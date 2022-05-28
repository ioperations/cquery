#include "lsp_diagnostic.h"
#include "match.h"
#include "working_files.h"

struct DiagnosticsEngine {
    void Init();
    void Publish(WorkingFiles* working_files, std::string path,
                 std::vector<lsDiagnostic> diagnostics);

    std::unique_ptr<GroupMatch> m_match;
    int64_t m_next_publish = 0;
    int m_frequency_ms;
};
