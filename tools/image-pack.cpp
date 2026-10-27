// image-pack <out.bin> <out.hex> <ddr bytes> <boot.bin> [<model.img> <model offset>]
//
// Lays the boot image at the start of the DDR and, when there is one, the
// model image at <model offset>, and writes the DDR image in the two forms the
// simulators take:
//   out.bin  sparse file, byte for byte what the DDR holds
//   out.hex  $readmemh, one byte per line as the P2E loader takes it, with
//            "@<byte offset>" at the start of every run of non-zero pages;
//            all-zero pages are left out
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint64_t page = 4096;

// Writes `path` at `offset` of the sparse bin, and its non-zero pages to hex.
void place(std::fstream &bin, std::ofstream &hex, const std::string &path,
           uint64_t offset, uint64_t ddr) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read " + path);
  in.seekg(0, std::ios::end);
  const uint64_t bytes = in.tellg();
  if (offset % page || offset + bytes > ddr)
    throw std::runtime_error(path + " does not fit the DDR at its offset");
  in.seekg(0);
  std::vector<char> data(page);
  bool continued = false;
  for (uint64_t done = 0; done < bytes; done += page) {
    const uint64_t length = std::min<uint64_t>(page, bytes - done);
    std::fill(data.begin(), data.end(), 0);
    in.read(data.data(), length);
    bool zero = true;
    for (uint64_t i = 0; i < length && zero; ++i)
      zero = data[i] == 0;
    if (zero) {
      continued = false;
      continue;
    }
    bin.seekp(offset + done);
    bin.write(data.data(), length);
    if (!continued)
      hex << '@' << std::hex << offset + done << std::dec << '\n';
    continued = true;
    for (uint64_t i = 0; i < page; ++i) {
      char digits[4];
      std::snprintf(digits, sizeof digits, "%02X\n",
                    static_cast<unsigned char>(data[i]));
      hex << digits;
    }
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 5 && argc != 7) {
    std::cerr << "usage: image-pack <out.bin> <out.hex> <ddr bytes> <boot.bin> "
                 "[<model.img> <model offset>]\n";
    return 1;
  }
  const uint64_t ddr = std::stoull(argv[3], nullptr, 0);
  std::fstream bin(argv[1], std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  std::ofstream hex(argv[2]);
  place(bin, hex, argv[4], 0, ddr);
  if (argc == 7)
    place(bin, hex, argv[5], std::stoull(argv[6], nullptr, 0), ddr);
  bin.seekp(ddr - 1);
  bin.put(0);
  if (!bin || !hex)
    throw std::runtime_error("cannot write the image");
}
