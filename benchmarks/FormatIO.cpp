/* Licensed under the MIT License. Matched owning API/text/binary construction costs. */
#include "../tests/Fixtures/UtilityPrograms.hpp"
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iostream>
using namespace mirnext;
namespace {
using Clock = std::chrono::steady_clock;
double elapsed (Clock::time_point start) {
  return std::chrono::duration<double, std::milli> (Clock::now () - start).count ();
}
struct File {
  std::FILE *value = std::tmpfile ();
  ~File () {
    if (value) std::fclose (value);
  }
};
Result<void> measure (unsigned count) {
  auto start = Clock::now ();
  auto context = std::make_unique<ir::Program> ();
  for (unsigned n = 0; n < count; ++n) {
    auto result = test::append_api_sieve (*context, "sieve" + std::to_string (n));
    if (!(result).has_value ()) return result.error ();
  }
  auto api_ms = elapsed (start);
  auto valid = context->verify ();
  if (!(valid).has_value ()) return valid.error ();
  start = Clock::now ();
  auto text = serialization::write_text (*context);
  if (!(text).has_value ()) return text.error ();
  auto text_write_ms = elapsed (start);
  start = Clock::now ();
  auto parsed
    = serialization::parse_text ((text).value (), {.max_tokens = std::size_t (count) * 4096,
                                                   .max_instructions = std::size_t (count) * 128});
  auto text_parse_ms = elapsed (start);
  if (!(parsed).has_value ()) return parsed.error ();
  auto canonical = serialization::write_text ((parsed).value ());
  if (!(canonical).has_value ()) return canonical.error ();
  if ((canonical).value () != (text).value ())
    return Error{ErrorCode::InvalidOperand, "API and parsed text differ"};
  context.reset ();
  start = Clock::now ();
  auto bytes = serialization::write_binary ((parsed).value ());
  if (!(bytes).has_value ()) return bytes.error ();
  auto write_ms = elapsed (start);
  (parsed).value () = ir::Program ();
  canonical.value ().clear ();
  canonical.value ().shrink_to_fit ();
  File file;
  if (!file.value)
    return Error{ErrorCode::InvalidArgument, "cannot create private benchmark temporary stream"};
  start = Clock::now ();
  if (std::fwrite (bytes.value ().data (), 1, bytes.value ().size (), file.value)
        != bytes.value ().size ()
      || std::fflush (file.value))
    return Error{ErrorCode::InvalidArgument, "benchmark temporary stream write failed"};
  auto file_write_ms = elapsed (start);
  std::rewind (file.value);
  std::vector<std::byte> input (bytes.value ().size ());
  start = Clock::now ();
  auto read = std::fread (input.data (), 1, input.size (), file.value);
  auto file_read_ms = elapsed (start);
  if (read != input.size () || std::ferror (file.value) || input != (bytes).value ())
    return Error{ErrorCode::InvalidArgument, "benchmark temporary stream read differs"};
  start = Clock::now ();
  auto decoded
    = serialization::read_binary (input, {.max_bytes = std::size_t (count) * 16384,
                                          .max_instructions = std::size_t (count) * 128});
  auto binary_read_ms = elapsed (start);
  if (!(decoded).has_value ()) return decoded.error ();
  auto reencoded = serialization::write_binary ((decoded).value ());
  if (!(reencoded).has_value ()) return reencoded.error ();
  if ((reencoded).value () != (bytes).value ()
      || std::size_t (
           std::distance (decoded.value ().modules ().begin (), decoded.value ().modules ().end ()))
           != count)
    return Error{ErrorCode::InvalidOperand, "binary construction differs from API/text"};
  std::cout << "functions=" << count << " api_build_ms=" << api_ms
            << " text_write_ms=" << text_write_ms << " text_parse_ms=" << text_parse_ms
            << " binary_write_ms=" << write_ms << " file_write_ms=" << file_write_ms
            << " file_read_ms=" << file_read_ms << " binary_read_ms=" << binary_read_ms
            << " text_bytes=" << text.value ().size () << " binary_bytes=" << bytes.value ().size ()
            << " binary_text_size_ratio=" << double (bytes.value ().size ()) / text.value ().size ()
            << '\n';
  return {};
}
}  // namespace
int main (int argc, char **argv) {
  unsigned count = 100000;
  if (argc != 1) {
    if (argc != 3 || std::string_view (argv[1]) != "--functions") {
      std::cerr << "usage: mirnext_format_io_benchmark [--functions N]\n";
      return 1;
    }
    std::string_view value (argv[2]);
    auto parsed = std::from_chars (value.data (), value.data () + value.size (), count);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data () + value.size () || !count
        || count > 1000000) {
      std::cerr << "invalid benchmark function count\n";
      return 1;
    }
  }
  auto result = measure (count);
  if (!(result).has_value ()) {
    std::cerr << result.error ().message << '\n';
    return 1;
  }
}
