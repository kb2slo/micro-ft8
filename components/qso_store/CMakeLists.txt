# The QSO store: every QSO as a record with an ID in one append-only journal,
# indexed in RAM (qso_store.h). No dependencies: the caller reads and appends
# the file, so it is host-tested as is.
idf_component_register(
    SRCS "qso_store.cpp" "qso_store_fields.cpp" "qso_store_journal.cpp" "qso_store_query.cpp"
    INCLUDE_DIRS "include"
)
