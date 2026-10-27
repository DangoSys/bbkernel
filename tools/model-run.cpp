// model-run <model base>
//
// Exits 2 when the model region holds no model.
//
// Runs the model in the model region of the DDR (see models/tools/model-pack.cpp):
// takes the runner library out of it and starts buddy-cli with it, with the
// options in the image's `run.args`, one per line. buddy-cli and this program
// are in the rootfs; the model, its weights and the runner are not.
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <resources.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: model-run <model base> [buddy-cli options]\n";
    return 1;
  }
  try {
    resources::configure(std::stoull(argv[1], nullptr, 0));
  } catch (const resources::NoImage &) {
    return 2;
  }
  std::string runner;
  for (const auto &[path, entry] : resources::image.files)
    if (path.ends_with(".so"))
      runner = path;
  if (runner.empty())
    throw std::runtime_error("the model image has no runner library");
  const auto bytes = resources::image.files.at(runner).bytes;
  const resources::Mapping library(runner, bytes, 8);
  const std::string file = "/tmp/" + runner;
  const int out = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
  if (out < 0 || write(out, library.data(), bytes) != ssize_t(bytes))
    throw std::runtime_error("cannot write " + file);
  close(out);

  std::vector<std::string> arguments{"buddy-cli", "--model-so", file,
                                     "--runner-so", file};
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
