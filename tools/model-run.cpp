// model-run <model base>
//
// Exits 2 when the model region holds no model.
//
// Runs the model in the model region of the DDR (see models/tools/model-pack.cpp):
// extracts its files and starts buddy-cli with the
// options in the image's `run.args`, one per line. buddy-cli and this program
// are in the rootfs; the model, its weights and the runner are not.
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <resources.h>
#include <string>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: model-run <model base>\n";
    return 1;
  }
  try {
    resources::configure(std::stoull(argv[1], nullptr, 0));
  } catch (const resources::NoImage &) {
    return 2;
  }
  std::filesystem::create_directories("/tmp/model");
  std::filesystem::current_path("/tmp/model");
  for (const auto &[path, entry] : resources::image.files) {
    if (entry.mapped)
      continue;
    const resources::Mapping data(path, entry.bytes, 8);
    if (std::filesystem::path(path).has_parent_path())
      std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(path, std::ios::binary);
    file.write(static_cast<const char *>(data.data()), entry.bytes);
    file.close();
  }
  std::vector<std::string> arguments{"buddy-cli"};
  const auto options = resources::image.files.at("run.args");
  const resources::Mapping text("run.args", options.bytes, 8);
  std::istringstream lines(
      std::string(static_cast<const char *>(text.data()), options.bytes));
  for (std::string line; std::getline(lines, line);)
    arguments.push_back(line);
  std::vector<char *> pointers;
  for (auto &argument : arguments)
    pointers.push_back(argument.data());
  pointers.push_back(nullptr);
  execv("/bin/buddy-cli", pointers.data());
  throw std::runtime_error("cannot start /bin/buddy-cli");
}
