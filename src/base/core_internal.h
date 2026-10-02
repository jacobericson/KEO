// core_internal.h - Log-file state shared by core.cpp and log.cpp.
// Private to core.cpp and log.cpp; log.cpp owns access under logCS.

#ifndef KEO_CORE_INTERNAL_H
#define KEO_CORE_INTERNAL_H

#include <string> // logFilePath
#include <fstream> // logFile

namespace core_detail
{ // Shared log-file declarations.
extern std::string logFilePath;
extern std::ofstream logFile;
} // namespace core_detail

#endif // KEO_CORE_INTERNAL_H
