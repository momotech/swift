set(SWIFT_GYB_FLAGS
    "-DCFDatabaseFile=${SWIFT_SOURCE_DIR}/lib/ClangImporter/CFDatabase.def")

add_gyb_target(generated_sorted_cf_database
    SortedCFDatabase.def.gyb
    DEPENDS "${SWIFT_SOURCE_DIR}/lib/ClangImporter/CFDatabase.def")

add_swift_host_library(swiftClangImporter STATIC
  CFTypeInfo.cpp
  ClangAdapter.cpp
  ClangDiagnosticConsumer.cpp
  ClangImporter.cpp
  ClangImporterRequests.cpp
  ClangModuleDependencyScanner.cpp
  ClangSourceBufferImporter.cpp
  DWARFImporter.cpp
  ImportDecl.cpp
  ImportEnumInfo.cpp
  ImportMacro.cpp
  ImportName.cpp
  ImportType.cpp
  Serializability.cpp
  SwiftLookupTable.cpp
)
target_link_libraries(swiftClangImporter PRIVATE
  swiftAST
  swiftParse
  clangTooling
  LLVMBitstreamReader)

target_link_libraries(swiftClangImporter INTERFACE
  clangDependencyScanning)

# This property is only set by calls to clang_tablegen. It will not be set on
# standalone builds, so it can always be safely passed.
get_property(CLANG_TABLEGEN_TARGETS GLOBAL PROPERTY CLANG_TABLEGEN_TARGETS)
add_dependencies(swiftClangImporter
  generated_sorted_cf_database
  ${CLANG_TABLEGEN_TARGETS})

set_swift_llvm_is_available(swiftClangImporter)
