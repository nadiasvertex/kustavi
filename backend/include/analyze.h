#pragma once

#include <filesystem>

namespace kustavi::cmd {
/** Initialize the database and workspace in the specified folder
 * @param folder_path The path to the folder where the source images live.
 */
void analyze(const std::filesystem::path &folder_path);
} // namespace kustavi::cmd
