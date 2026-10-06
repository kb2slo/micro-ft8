#include "storage_service.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

constexpr char kPartitionLabel[] = "storage";
constexpr char kBasePath[] = "/storage";
constexpr char kStationFile[] = "Station.txt";
constexpr char kStationTemp[] = "Station.tmp";
const char* TAG = "storage_service";

StaticSemaphore_t s_mutex_buffer;
SemaphoreHandle_t s_mutex;
StorageOwner s_owner = StorageOwner::UNAVAILABLE;
bool s_lfs_mounted = false;
size_t s_open_streams;

class StorageGuard {
public:
    StorageGuard() : held_(s_mutex && xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY) == pdTRUE) {}
    ~StorageGuard() {
        if (held_) {
            xSemaphoreGiveRecursive(s_mutex);
        }
    }
    bool held() const { return held_; }

private:
    bool held_;
};

bool firmware_owns_storage_locked() {
    return s_owner == StorageOwner::FIRMWARE && s_lfs_mounted;
}

bool normalize_name(const std::string& input, std::string& name) {
    name = input;
    const std::string prefix = std::string(kBasePath) + "/";
    if (name.compare(0, prefix.size(), prefix) == 0) {
        name.erase(0, prefix.size());
    }
    if (name.empty() || name.front() == '/' || name.find('\\') != std::string::npos ||
        name.find("..") != std::string::npos) {
        return false;
    }
    return true;
}

bool build_path(const std::string& input, std::string& path) {
    std::string name;
    if (!normalize_name(input, name)) {
        return false;
    }
    path = std::string(kBasePath) + "/" + name;
    return true;
}

bool sync_fd(int fd) {
    if (fd < 0) {
        return false;
    }
    if (fsync(fd) == 0) {
        return true;
    }
    // Some VFS FAT fds report ENOTSUP; caller must still have flushed writes.
    return errno == ENOTSUP || errno == EINVAL;
}

bool sync_file(FILE* file) {
    if (!file) {
        return false;
    }
    if (fflush(file) != 0) {
        return false;
    }
    return sync_fd(fileno(file));
}

// Temp names stay short (legacy FAT 8.3 habit); LittleFS allows longer.
std::string make_temp_name(const std::string& name) {
    if (name == kStationFile) {
        return kStationTemp;
    }
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && dot <= 8 &&
        name.size() - dot - 1 <= 3 && name.find('.', dot + 1) == std::string::npos) {
        return name.substr(0, dot) + ".tmp";
    }
    std::string base = name.substr(0, std::min<size_t>(name.size(), 8));
    return base + ".tmp";
}

bool flush_all_locked() {
    errno = 0;
    const int rc = fflush(nullptr);
    const int err_no = errno;
    ESP_LOGI(TAG, "flush: rc=%d errno=%d (%s)",
             rc, err_no, strerror(err_no));
    return rc == 0;
}

bool commit_temp_path(const std::string& final_path, const std::string& temp_path) {
    if (unlink(final_path.c_str()) != 0 && errno != ENOENT) {
        unlink(temp_path.c_str());
        return false;
    }
    if (rename(temp_path.c_str(), final_path.c_str()) != 0) {
        ESP_LOGE(TAG, "rename failed: %s -> %s", temp_path.c_str(), final_path.c_str());
        unlink(temp_path.c_str());
        return false;
    }
    return true;
}

bool write_atomic_file(const std::string& final_path,
                       const std::string& temp_path,
                       const std::string& content) {
    FILE* file = fopen(temp_path.c_str(), "wb");
    if (!file) {
        return false;
    }

    bool ok = content.empty() || fwrite(content.data(), 1, content.size(), file) == content.size();
    ok = ok && sync_file(file);
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(temp_path.c_str());
        return false;
    }

    return commit_temp_path(final_path, temp_path);
}

bool write_atomic_locked(const std::string& input, const std::string& content) {
    if (!firmware_owns_storage_locked()) {
        return false;
    }

    std::string name;
    std::string final_path;
    if (!normalize_name(input, name) || !build_path(name, final_path)) {
        return false;
    }
    const std::string temp_name = make_temp_name(name);
    std::string temp_path;
    if (!build_path(temp_name, temp_path)) {
        return false;
    }

    return write_atomic_file(final_path, temp_path, content);
}

bool list_files_locked(std::vector<std::string>& files) {
    files.clear();
    if (!firmware_owns_storage_locked()) {
        return false;
    }

    DIR* dir = opendir(kBasePath);
    if (!dir) {
        return false;
    }

    while (dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (!name || name[0] == '.') {
            continue;
        }
        std::string path;
        if (!build_path(name, path)) {
            continue;
        }
        struct stat info {};
        if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
            files.emplace_back(name);
        }
    }
    closedir(dir);
    return true;
}

bool cabrillo_ensure_header_locked(const std::string& path,
                                   const std::string& mycall,
                                   const std::string& location) {
    struct stat info {};
    if (stat(path.c_str(), &info) == 0 && info.st_size > 0) {
        return true;
    }

    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    bool ok = fprintf(file, "START-OF-LOG: 3.0\n") >= 0;
    ok = ok && fprintf(file, "CREATED-BY: Micro-FT8\n") >= 0;
    ok = ok && fprintf(file, "CONTEST: ARRL-FIELD-DAY\n") >= 0;
    ok = ok && fprintf(file, "CALLSIGN: %s\n", mycall.c_str()) >= 0;
    ok = ok && fprintf(file, "CATEGORY-OPERATOR: SINGLE-OP\n") >= 0;
    ok = ok && fprintf(file, "CATEGORY-TRANSMITTER: ONE\n") >= 0;
    ok = ok && fprintf(file, "CATEGORY-ASSISTED: NON-ASSISTED\n") >= 0;
    ok = ok && fprintf(file, "CATEGORY-BAND: ALL\n") >= 0;
    ok = ok && fprintf(file, "CATEGORY-MODE: MIXED\n") >= 0;
    ok = ok && fprintf(file, "CATEGORY-POWER: LOW\n") >= 0;
    ok = ok && fprintf(file, "CATEGORY-STATION: PORTABLE\n") >= 0;
    ok = ok && fprintf(file, "LOCATION: %s\n", location.c_str()) >= 0;
    ok = ok && fprintf(file, "OPERATORS: %s\n", mycall.c_str()) >= 0;
    ok = ok && fprintf(file, "END-OF-LOG:\n") >= 0;
    ok = ok && sync_file(file);
    if (fclose(file) != 0) {
        ok = false;
    }
    return ok;
}

bool cabrillo_truncate_end_marker(FILE* file) {
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        return false;
    }
    const long file_end = ftell(file);
    if (file_end <= 0) {
        return false;
    }

    constexpr long kMaxTail = 256;
    const long tail_start = file_end > kMaxTail ? file_end - kMaxTail : 0;
    if (fseek(file, tail_start, SEEK_SET) != 0) {
        return false;
    }

    std::string tail(static_cast<size_t>(file_end - tail_start), '\0');
    tail.resize(fread(tail.data(), 1, tail.size(), file));
    size_t line_end = tail.size();
    while (line_end > 0 && (tail[line_end - 1] == '\n' || tail[line_end - 1] == '\r')) {
        --line_end;
    }
    if (line_end == 0) {
        return false;
    }
    size_t line_start = tail.rfind('\n', line_end - 1);
    line_start = line_start == std::string::npos ? 0 : line_start + 1;
    if (tail.substr(line_start, line_end - line_start) != "END-OF-LOG:") {
        return false;
    }

    const int descriptor = fileno(file);
    const long truncate_at = tail_start + static_cast<long>(line_start);
    return descriptor >= 0 && ftruncate(descriptor, truncate_at) == 0 &&
           fseek(file, 0, SEEK_END) == 0;
}

}  // namespace

struct StorageStream {
    FILE* file = nullptr;
};

esp_err_t storage_service_init() {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mutex_buffer);
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    StorageGuard guard;
    if (!guard.held()) {
        return ESP_FAIL;
    }
    if (s_owner != StorageOwner::UNAVAILABLE) {
        return ESP_OK;
    }

    // I29: LittleFS on partition label "storage" (was FAT + TinyUSB MSC on "fatfs").
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = kBasePath;
    conf.partition_label = kPartitionLabel;
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed on '%s': %s", kPartitionLabel, esp_err_to_name(err));
        return err;
    }

    s_lfs_mounted = true;
    s_owner = StorageOwner::FIRMWARE;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(kPartitionLabel, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "initialized LittleFS %s on '%s' (%u / %u bytes used)", kBasePath,
                 kPartitionLabel, (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "initialized LittleFS %s on '%s'", kBasePath, kPartitionLabel);
    }
    return ESP_OK;
}

StorageOwner storage_service_owner() {
    StorageGuard guard;
    return guard.held() ? s_owner : StorageOwner::UNAVAILABLE;
}

bool storage_service_firmware_available() {
    StorageGuard guard;
    return guard.held() && firmware_owns_storage_locked();
}

bool storage_service_info(size_t* total_out, size_t* used_out) {
    StorageGuard guard;
    if (!guard.held() || !firmware_owns_storage_locked()) {
        return false;
    }
    size_t total = 0, used = 0;
    if (esp_littlefs_info(kPartitionLabel, &total, &used) != ESP_OK) {
        return false;
    }
    if (total_out) *total_out = total;
    if (used_out) *used_out = used;
    return true;
}

size_t storage_service_open_stream_count() {
    StorageGuard guard;
    return guard.held() ? s_open_streams : 0;
}

bool storage_file_exists(const std::string& name) {
    StorageGuard guard;
    std::string path;
    struct stat info {};
    return guard.held() && firmware_owns_storage_locked() && build_path(name, path) &&
           stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool storage_file_remove(const std::string& name) {
    StorageGuard guard;
    std::string path;
    return guard.held() && firmware_owns_storage_locked() && build_path(name, path) &&
           unlink(path.c_str()) == 0;
}

bool storage_file_list(std::vector<std::string>& files) {
    StorageGuard guard;
    return guard.held() && list_files_locked(files);
}

bool storage_file_read_text(const std::string& name, std::string& content) {
    StorageGuard guard;
    content.clear();
    std::string path;
    if (!guard.held() || !firmware_owns_storage_locked() || !build_path(name, path)) {
        return false;
    }

    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    char buffer[512];
    while (true) {
        const size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count > 0) {
            content.append(buffer, count);
        }
        if (count < sizeof(buffer)) {
            break;
        }
    }
    const bool ok = ferror(file) == 0;
    fclose(file);
    return ok;
}

bool storage_file_write_atomic(const std::string& name, const std::string& content) {
    StorageGuard guard;
    return guard.held() && write_atomic_locked(name, content);
}

bool storage_file_append(const std::string& name,
                         const std::string& content,
                         const std::string& header_if_new,
                         bool sync_to_flash) {
    StorageGuard guard;
    std::string path;
    if (!guard.held() || !firmware_owns_storage_locked() || !build_path(name, path)) {
        ESP_LOGW(TAG, "append skipped %s held=%d owner=%d",
                 name.c_str(), (int)guard.held(), (int)s_owner);
        return false;
    }
    (void)sync_to_flash;  // always sync; durable across power loss

    struct stat before {};
    const bool existed = stat(path.c_str(), &before) == 0 && S_ISREG(before.st_mode);
    const off_t size_before = existed ? before.st_size : 0;
    const bool need_header =
        !header_if_new.empty() && (!existed || size_before == 0);
    const off_t expected_grow =
        static_cast<off_t>((need_header ? header_if_new.size() : 0) + content.size());

    // Empty/missing files: same path as Station.txt (temp + fsync + rename).
    // fopen("ab") was creating a 0-byte dirent that never grew on this volume.
    if (size_before == 0) {
        std::string payload;
        payload.reserve(static_cast<size_t>(expected_grow));
        if (need_header) {
            payload.append(header_if_new);
        }
        payload.append(content);
        if (!write_atomic_locked(name, payload)) {
            ESP_LOGE(TAG, "append atomic create failed %s", path.c_str());
            return false;
        }
        struct stat after {};
        const bool sized = stat(path.c_str(), &after) == 0 && S_ISREG(after.st_mode);
        const off_t size_after = sized ? after.st_size : -1;
        if (expected_grow > 0 && (!sized || size_after < expected_grow)) {
            ESP_LOGE(TAG, "append atomic size mismatch %s after=%ld expected=%ld",
                     path.c_str(), (long)size_after, (long)expected_grow);
            return false;
        }
        ESP_LOGI(TAG, "append atomic ok %s -> %ld", path.c_str(), (long)size_after);
        return true;
    }

    // Growing an existing file: POSIX append + fsync (avoid stdio "ab").
    const int fd = open(path.c_str(), O_WRONLY | O_APPEND, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "append open failed %s errno=%d", path.c_str(), errno);
        return false;
    }
    bool ok = true;
    if (need_header && !header_if_new.empty()) {
        ok = write(fd, header_if_new.data(), header_if_new.size()) ==
             static_cast<ssize_t>(header_if_new.size());
    }
    if (ok && !content.empty()) {
        ok = write(fd, content.data(), content.size()) ==
             static_cast<ssize_t>(content.size());
    }
    ok = ok && sync_fd(fd);
    if (close(fd) != 0) {
        ok = false;
    }

    struct stat after {};
    const bool sized = stat(path.c_str(), &after) == 0 && S_ISREG(after.st_mode);
    const off_t size_after = sized ? after.st_size : -1;
    if (ok && expected_grow > 0 && (!sized || size_after < size_before + expected_grow)) {
        ESP_LOGE(TAG, "append size mismatch %s before=%ld after=%ld expected+=%ld",
                 path.c_str(), (long)size_before, (long)size_after, (long)expected_grow);
        ok = false;
    }
    if (!ok) {
        ESP_LOGE(TAG, "append failed %s errno=%d before=%ld after=%ld",
                 path.c_str(), errno, (long)size_before, (long)size_after);
    } else {
        ESP_LOGI(TAG, "append ok %s %ld -> %ld", path.c_str(), (long)size_before,
                 (long)size_after);
    }
    return ok;
}

bool storage_file_append_cabrillo(const std::string& mycall,
                                  const std::string& location,
                                  const std::string& qso_line) {
    StorageGuard guard;
    std::string path;
    if (!guard.held() || !firmware_owns_storage_locked() ||
        !build_path("fieldday.txt", path) ||
        !cabrillo_ensure_header_locked(path, mycall, location)) {
        return false;
    }

    FILE* file = fopen(path.c_str(), "r+b");
    if (!file) {
        file = fopen(path.c_str(), "a+b");
    }
    if (!file) {
        return false;
    }

    cabrillo_truncate_end_marker(file);
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long end = ftell(file);
    if (end > 0 && fseek(file, -1, SEEK_END) == 0) {
        const int last = fgetc(file);
        fseek(file, 0, SEEK_END);
        if (last != '\n') {
            fputc('\n', file);
        }
    }
    bool ok = fprintf(file, "%s\nEND-OF-LOG:\n", qso_line.c_str()) >= 0;
    ok = ok && sync_file(file);
    if (fclose(file) != 0) {
        ok = false;
    }
    return ok;
}

StorageStream* storage_stream_open(const std::string& name, StorageOpenMode mode) {
    StorageGuard guard;
    std::string path;
    if (!guard.held() || !firmware_owns_storage_locked() || !build_path(name, path)) {
        return nullptr;
    }

    const char* open_mode = "rb";
    if (mode == StorageOpenMode::WRITE_TRUNCATE) {
        open_mode = "wb";
    } else if (mode == StorageOpenMode::APPEND) {
        open_mode = "ab";
    }

    StorageStream* stream = new (std::nothrow) StorageStream;
    if (!stream) {
        return nullptr;
    }
    stream->file = fopen(path.c_str(), open_mode);
    if (!stream->file) {
        delete stream;
        return nullptr;
    }
    ++s_open_streams;
    return stream;
}

size_t storage_stream_read(StorageStream* stream, void* data, size_t size) {
    StorageGuard guard;
    if (!guard.held() || !firmware_owns_storage_locked() || !stream || !stream->file) {
        return 0;
    }
    return fread(data, 1, size, stream->file);
}

bool storage_stream_read_line(StorageStream* stream, char* line, size_t line_size) {
    StorageGuard guard;
    return guard.held() && firmware_owns_storage_locked() && stream && stream->file &&
           line && line_size > 0 && fgets(line, static_cast<int>(line_size), stream->file);
}

size_t storage_stream_write(StorageStream* stream, const void* data, size_t size) {
    StorageGuard guard;
    if (!guard.held() || !firmware_owns_storage_locked() || !stream || !stream->file) {
        return 0;
    }
    return fwrite(data, 1, size, stream->file);
}

bool storage_stream_seek(StorageStream* stream, long offset, int whence) {
    StorageGuard guard;
    return guard.held() && firmware_owns_storage_locked() && stream && stream->file &&
           fseek(stream->file, offset, whence) == 0;
}

long storage_stream_tell(StorageStream* stream) {
    StorageGuard guard;
    if (!guard.held() || !firmware_owns_storage_locked() || !stream || !stream->file) {
        return -1;
    }
    return ftell(stream->file);
}

long storage_stream_size(StorageStream* stream) {
    StorageGuard guard;
    if (!guard.held() || !firmware_owns_storage_locked() || !stream || !stream->file) {
        return -1;
    }
    const long current = ftell(stream->file);
    if (current < 0 || fseek(stream->file, 0, SEEK_END) != 0) {
        return -1;
    }
    const long size = ftell(stream->file);
    fseek(stream->file, current, SEEK_SET);
    return size;
}

bool storage_stream_sync(StorageStream* stream) {
    StorageGuard guard;
    return guard.held() && firmware_owns_storage_locked() && stream && stream->file &&
           sync_file(stream->file);
}

void storage_stream_close(StorageStream* stream) {
    if (!stream) {
        return;
    }
    StorageGuard guard;
    if (guard.held()) {
        if (stream->file) {
            fclose(stream->file);
            stream->file = nullptr;
        }
        if (s_open_streams > 0) {
            --s_open_streams;
        }
    }
    delete stream;
}

bool storage_service_flush_all() {
    StorageGuard guard;
    if (!guard.held() || !firmware_owns_storage_locked()) {
        return false;
    }
    return flush_all_locked();
}
