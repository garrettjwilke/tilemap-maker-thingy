#include "nfd.h"
#include "web_file_io.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static char s_last_error[256] = "";

extern "C" {

nfdresult_t NFD_Init(void) {
    s_last_error[0] = '\0';
    return NFD_OKAY;
}

void NFD_Quit(void) {
}

void NFD_FreePathU8(nfdu8char_t* path) {
    if (path) {
        free(path);
    }
}

const char* NFD_GetError(void) {
    return s_last_error;
}

nfdresult_t NFD_OpenDialogU8(nfdu8char_t** outPath, const nfdu8filteritem_t* filterList, nfdfiltersize_t count, const nfdu8char_t* defaultPath) {
    (void)defaultPath;
    if (!outPath) return NFD_ERROR;
    *outPath = nullptr;

    std::string accept;
    for (nfdfiltersize_t i = 0; i < count; ++i) {
        if (filterList && filterList[i].spec) {
            std::string spec = filterList[i].spec;
            size_t start = 0;
            while (start < spec.size()) {
                size_t comma = spec.find(',', start);
                std::string ext = (comma == std::string::npos) ? spec.substr(start) : spec.substr(start, comma - start);
                if (!ext.empty()) {
                    if (!accept.empty()) accept += ",";
                    if (ext[0] != '.') accept += ".";
                    accept += ext;
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
    }

#ifdef __EMSCRIPTEN__
    tmm::web_trigger_file_dialog(accept.c_str(), tmm::WebFileTarget_Auto);
#endif
    return NFD_CANCEL; // Asynchronous flow handled via callback
}

nfdresult_t NFD_SaveDialogU8(nfdu8char_t** outPath, const nfdu8filteritem_t* filterList, nfdfiltersize_t count, const nfdu8char_t* defaultPath, const nfdu8char_t* defaultName) {
    (void)filterList;
    (void)count;
    (void)defaultPath;
    if (!outPath) return NFD_ERROR;

    std::string name = (defaultName && defaultName[0]) ? defaultName : "export";
    std::string full_path = "/downloads/" + name;

    std::error_code ec;
    fs::create_directories("/downloads", ec);

    *outPath = strdup(full_path.c_str());
    return NFD_OKAY;
}

nfdresult_t NFD_PickFolderU8(nfdu8char_t** outPath, const nfdu8char_t* defaultPath) {
    (void)defaultPath;
    if (!outPath) return NFD_ERROR;

    std::error_code ec;
    fs::create_directories("/downloads", ec);

    *outPath = strdup("/downloads");
    return NFD_OKAY;
}

} // extern "C"
