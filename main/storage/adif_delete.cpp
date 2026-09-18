#include "adif_delete.h"

#include "qso_browse.h"
#include "storage_service.h"

namespace {

bool name_ok_basename(const char* name) {
    if (!name || name[0] == '\0') {
        return false;
    }
    for (const char* p = name; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* adif_delete_plan(const char* filename,
                             const std::vector<std::string>& all_names,
                             AdifDeleteIsLockedFn is_locked,
                             std::vector<std::string>* to_delete) {
    if (!to_delete) {
        return "internal";
    }
    to_delete->clear();

    std::vector<std::string> daily;
    qso_browse_select_daily_files(all_names, &daily);

    const bool clear_all = !filename || filename[0] == '\0';
    if (clear_all) {
        bool any_locked = false;
        for (const std::string& name : daily) {
            if (is_locked && is_locked(name.c_str())) {
                any_locked = true;
                continue;
            }
            to_delete->push_back(name);
        }
        if (to_delete->empty() && any_locked) {
            return "active log protected";
        }
        return nullptr;
    }

    if (!name_ok_basename(filename) || !qso_browse_is_daily_log_name(filename)) {
        return "not a daily log";
    }
    if (is_locked && is_locked(filename)) {
        return "active log protected";
    }
    bool found = false;
    for (const std::string& name : daily) {
        if (name == filename) {
            found = true;
            break;
        }
    }
    if (!found) {
        return "not found";
    }
    to_delete->push_back(filename);
    return nullptr;
}

const char* adif_delete_daily(const char* filename, AdifDeleteIsLockedFn is_locked) {
    std::vector<std::string> all_names;
    if (!storage_file_list(all_names)) {
        return "storage unavailable";
    }
    std::vector<std::string> to_delete;
    if (const char* why = adif_delete_plan(filename, all_names, is_locked, &to_delete)) {
        return why;
    }
    for (const std::string& name : to_delete) {
        if (!storage_file_remove(name)) {
            return "delete failed";
        }
    }
    return nullptr;
}
