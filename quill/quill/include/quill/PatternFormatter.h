/**
 * Copyright(c) 2020-present, Odysseas Georgoudis & quill contributors.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include "quill/bundled/fmt/format.h"
#include "quill/bundled/invoke/invoke.h"
#include "quill/detail/misc/Attributes.h"
#include "quill/detail/misc/Common.h"
#include "quill/detail/misc/Os.h"
#include "quill/detail/misc/TypeTraits.h"
#include "quill/detail/misc/Utilities.h"
#include "quill/detail/record/StaticLogRecordInfo.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

/**
 * A macro to pass a string as a constexpr argument
 * Must be used in order to pass the arguments to the set_pattern function
 */
#define QUILL_STRING(str)                                                                          \
  [] {                                                                                             \
    using R = union X {                                                                            \
      static constexpr auto value() { return str; }                                                \
    };                                                                                             \
    return R{};                                                                                    \
  }()

namespace quill
{
/**
 * Formats a log record based on a pattern provided by the user to an fmt::memory_buffer
 * The user provided pattern is broken down to three parts with part_2 being the actual %(message)
 * We are based on fmt.format() to format each one of the three parts
 *
 * The default pattern for example has
 * part 1: %(ascii_time) [%(thread)] %(filename):%(lineno) %(level_name) %(logger_name) -
 * part 2: %(message)
 * part 3: empty
 *
 * For part 1 and part 3
 * a) we will generate a tuple of callbacks in the same order as the format specifier
 * b) we will generate a fmt_string "{} [{}]] {}:{} {} {} - "
 * Those two are stored in a pre-generated tuple which is captured and accessed via the FormaterHelperBase
 *
 * To format a log message the process is to  :
 *
 * fmt.format(part_1)
 * fmt.format(log_record)
 * fmt.format(part_3);
 *
 * @note: Default pattern is "%(ascii_time) [%(thread)] %(filename):%(lineno) %(level_name) %(logger_name) - %(message)"
 */
class PatternFormatter
{
private:
  /**
   * A helper class to store the type of the generated tuple of callbacks for part_1 and part_3 of
   * the given pattern and access it via a virtual function.
   */
  class FormatterHelperBase
  {
  public:
    FormatterHelperBase() = default;
    virtual ~FormatterHelperBase() = default;

    /**
     * Appends information to memory_buffer based on the preconfigured pattern
     * @param[out] memory_buffer the memory buffer to write
     * @param timestamp
     * @param thread_id
     * @param logger_name
     * @param logline_info
     */
    virtual void format(fmt::memory_buffer& memory_buffer,
                        std::chrono::nanoseconds timestamp,
                        char const* thread_id,
                        char const* logger_name,
                        detail::StaticLogRecordInfo const& logline_info) const = 0;
  };

  /**
   * A templated derived class to capture and type erase the generated tuple
   * @tparam TTuple
   */
  template <typename TTuple>
  class FormatterHelper final : public FormatterHelperBase
  {
  public:
    FormatterHelper(TTuple tuple_of_callbacks, std::string fmt_pattern)
      : _tuple_of_callbacks(std::move(tuple_of_callbacks)), _fmt_pattern(std::move(fmt_pattern)){};

    ~FormatterHelper() override = default;

    /**
     * @see FormatterHelper::format
     * @param memory_buffer
     * @param timestamp
     * @param thread_id
     * @param logger_name
     * @param logline_info
     */
    void format(fmt::memory_buffer& memory_buffer,
                std::chrono::nanoseconds timestamp,
                char const* thread_id,
                char const* logger_name,
                detail::StaticLogRecordInfo const& logline_info) const override
    {
      // lambda expand the stored tuple arguments
      auto format_buffer = [this, &memory_buffer, timestamp, thread_id, logger_name,
                            logline_info](auto... tuple_args) {
        fmt::format_to(memory_buffer, _fmt_pattern.data(),
                       tuple_args(timestamp, thread_id, logger_name, logline_info)...);
      };

      invoke_hpp::apply(format_buffer, _tuple_of_callbacks);
    }

  private:
    TTuple _tuple_of_callbacks;
    std::string _fmt_pattern;
  };

  /** Public classes **/
public:
  /**
   * Stores the timezone that is used for the timestamp
   */
  enum class Timezone : uint8_t
  {
    LocalTime,
    GmtTime
  };

  /**
   * Stores the precission of the timestamp
   */
  enum class TimestampPrecision : uint8_t
  {
    None,
    MilliSeconds,
    MicroSeconds,
    NanoSeconds
  };

  /** Main PatternFormatter class **/
public:
  /**
   * Constructor
   */
  PatternFormatter()
  {
    // Set the default pattern
    _set_pattern(
      QUILL_STRING("%(ascii_time) [%(thread)] %(filename):%(lineno) LOG_%(level_name) "
                   "%(logger_name) - %(message)"));
  }

  /**
   * Constructor for a PatterFormater with a custom format
   * @tparam TConstantString
   * @param format_pattern format_pattern a format string. Must be passed using the macro QUILL_STRING("format string");
   * @param date_format The for format of the date. Same as strftime() format.
   * @param timezone The timezone of the timestamp
   */
  template <typename TConstantString>
  PatternFormatter(TConstantString format_pattern, std::string date_format, TimestampPrecision timestamp_precision, Timezone timezone)
  {
    _set_pattern(format_pattern);

    _date_format = std::move(date_format);
    _timestamp_precision = timestamp_precision;
    _timezone_type = timezone;
  }

  /**
   * Deleted because during construction the PatternFormatter will create some lambdas that store a
   * reference to "this" as "this" being the PatternFormatter instance itself. After the object is
   * created we can not move it or copy it to a new instance all the lambdas have to be re-created.
   * To re-create the lambdas we need the compile time string
   * @param other
   */
  PatternFormatter(PatternFormatter const& other) = delete;
  PatternFormatter(PatternFormatter&& other) noexcept = delete;
  PatternFormatter& operator=(PatternFormatter const& other) = delete;
  PatternFormatter& operator=(PatternFormatter&& other) noexcept = delete;

  /**
   * Destructor
   */
  ~PatternFormatter() = default;

#if !defined(_WIN32)
  /**
   * Formats the given LogRecord
   * @tparam Args
   * @param timestamp
   * @param thread_id
   * @param logger_name
   * @param logline_info
   * @param args
   * @return a fmt::memory_buffer that contains the formatted log record
   */
  template <typename... Args>
  void format(std::chrono::nanoseconds timestamp,
              char const* thread_id,
              char const* logger_name,
              detail::StaticLogRecordInfo const& logline_info,
              Args const&... args) const;
#else
  /**
   * Formats the given LogRecord
   * Enabled only when there are no wide strings as arguments
   * @tparam Args
   * @param timestamp
   * @param thread_id
   * @param logger_name
   * @param logline_info
   * @param args
   * @return a fmt::memory_buffer that contains the formatted log record
   */
  template <typename... Args>
  typename std::enable_if_t<!detail::any_is_same<std::wstring, void, Args...>::value, void> format(
    std::chrono::nanoseconds timestamp,
    char const* thread_id,
    char const* logger_name,
    detail::StaticLogRecordInfo const& logline_info,
    Args const&... args) const;

  /**
   * Formats the given LogRecord after converting the wide characters to UTF-8
   * Enabled when a wide character is passed as an argument
   * @tparam Args
   * @param timestamp
   * @param thread_id
   * @param logger_name
   * @param logline_info
   * @param args
   * @return
   */
  template <typename... Args>
  typename std::enable_if_t<detail::any_is_same<std::wstring, void, Args...>::value, void> format(
    std::chrono::nanoseconds timestamp,
    char const* thread_id,
    char const* logger_name,
    detail::StaticLogRecordInfo const& logline_info,
    Args const&... args) const;
#endif

  /**
   * Returned the stored formatted record, to be called after format
   * @return
   */
  fmt::memory_buffer const& formatted_log_record() const noexcept { return _formatted_log_record; }

private:
  /**
   * The stored callback type that will return the appropriate value based on the format pattern specifiers
   */
  using argument_callback_t =
    std::function<char const*(std::chrono::nanoseconds, char const*, char const*, detail::StaticLogRecordInfo const&)>;

  /**
   * Generate a tuple of callbacks [](size i) { };
   * @tparam T
   * @tparam Is
   * @param callback
   * @return
   */
  template <typename TCallback, size_t... Is>
  QUILL_NODISCARD static auto _generate_tuple_impl(TCallback callback, std::index_sequence<Is...>)
  {
    return std::make_tuple(callback(Is)...);
  }

  /**
   * Generate a tuple of callbacks [](size i) { };
   * @tparam TCallback
   * @tparam N
   * @param callback
   * @return
   */
  template <size_t N, typename TCallback>
  QUILL_NODISCARD static auto _generate_tuple(TCallback callback)
  {
    return _generate_tuple_impl(callback, std::make_index_sequence<N>{});
  }

  /**
   * Sets a pattern to the formatter.
   * The given pattern is broken down into three parts : part_1 - %(message) - part_3
   *
   * E.g. given part_1 as %(logger) [%(thread)] we will
   * a) generate a tuple of two callbacks, callback with index 0 will return logger.name(), callback with index 1 will return thread.name();
   * b) generate a equivalent string for the fmt library in this case {} [{}]
   * c) pass to fmt.format the generated string and tuple of callbacks and let fmt library deal with the format
   *
   * Because we can't capture the generated tuple of callbacks we store it in a derived class and access it via
   * a virtual function
   *
   * The following attribute names can be used with the corresponding placeholder in a %-style format string.
   *
   * %(ascii_time)    - Human-readable time when the LogRecord was created
   * %(filename)      - Source file where the logging call was issued
   * %(pathname)      - Full source file where the logging call was issued
   * %(function_name) - Name of function containing the logging call
   * %(level_name)    - Text logging level for the messageText logging level for the message
   * %(lineno)        - Source line number where the logging call was issued
   * %(logger_name)   - Name of the logger used to log the call.
   * %(message)       - The logged message
   * %(thread)        - Thread ID
   *
   * @tparam TConstantString
   * @param format_pattern a format string. Must be passed using the macro QUILL_STRING("format string");
   */
  template <typename TConstantString>
  void _set_pattern(TConstantString);

  /** Count the number of '%' occurrences
   * The pattern is broken down in 3 parts
   * eg. %(logger) %(file) %(message) %(function)
   * Would be :
   * part 1 : %(logger) %(file)
   * part 2 : %(message)
   * part 3 : %(function)
   * We care only about part 1 and part 3 and part 2 we can ignore
   * @note: we always expect to find %(message)
   * */
  template <typename TConstantString>
  QUILL_NODISCARD static constexpr std::array<size_t, 2> _parse_format_pattern();

  /**
   * Process part of the pattern and create a helper formatter class
   * @tparam N
   * @param format_pattern_part
   * @return
   */
  template <size_t N>
  QUILL_NODISCARD std::unique_ptr<FormatterHelperBase> _make_formatter_helper(std::string const& format_pattern_part);

  /**
   * Given a message part generate a vector of callbacks in the excact same order as the format
   * specifiers in the pattern format string
   */
  QUILL_NODISCARD std::vector<argument_callback_t> _generate_vector_of_callbacks(std::string const& pattern);

  /**
   * Create the callback we need to use based on the format specifier
   * @param pattern_attr
   * @return
   */
  QUILL_NODISCARD argument_callback_t _select_argument_callback(std::string const& pattern_attr);

  /**
   * Given a part of the pattern modify it to match the expected lib fmt format
   */
  QUILL_NODISCARD std::string _generate_fmt_format_string(std::string pattern);

  /**
   * Formats an epoch timestamp from nanos to a local date.
   * Default is %H:%M:%S
   * @param epoch timestamp in nanoseconds from epoch. This timestamp must be in nanoseconds
   * @return formated date as a string
   */
  void _convert_epoch_to_local_date(std::chrono::nanoseconds epoch_time);

private:
  /** Formatters for any user's custom pattern - Important they have to be destructed last so they are defined first **/
  std::unique_ptr<FormatterHelperBase> _pattern_formatter_helper_part_1; /**< Formatter before %(message) **/
  std::unique_ptr<FormatterHelperBase> _pattern_formatter_helper_part_3; /**< Formatter after %(message) **/

  /** Strings as class members to avoid re-allocating **/
  using date_memory_buffer = fmt::basic_memory_buffer<char, 32>;
  date_memory_buffer _formatted_date;

  /** The buffer where we store each formatted string, also stored as class member to avoid
   * re-allocations. This is mutable so we can have a format() const function **/
  mutable fmt::memory_buffer _formatted_log_record;

#if defined(_WIN32)
  /** Windows support for wide characters, we need to store extra buffers to convert wide strings to utf8 before passing them to fmt format as narrow characters**/
  mutable fmt::wmemory_buffer _w_memory_buffer;
#endif

  std::string _date_format{"%H:%M:%S"};         /** Timestamp format **/
  Timezone _timezone_type{Timezone::LocalTime}; /** Timezone, GMT time by default **/
  TimestampPrecision _timestamp_precision{TimestampPrecision::NanoSeconds}; /** timestamp precision */
};

/** Inline Implementation **/

#if !defined(_WIN32)
/***/
template <typename... Args>
void PatternFormatter::format(std::chrono::nanoseconds timestamp,
                              const char* thread_id,
                              char const* logger_name,
                              detail::StaticLogRecordInfo const& logline_info,
                              Args const&... args) const
{
  // clear out existing buffer
  _formatted_log_record.clear();

  // Format part 1 of the pattern first
  if (_pattern_formatter_helper_part_1)
  {
    _pattern_formatter_helper_part_1->format(_formatted_log_record, timestamp, thread_id, logger_name, logline_info);
  }

  // Format the user requested string
  fmt::format_to(_formatted_log_record, logline_info.message_format(), args...);

  // Format part 3 of the pattern
  if (_pattern_formatter_helper_part_3)
  {
    _pattern_formatter_helper_part_3->format(_formatted_log_record, timestamp, thread_id, logger_name, logline_info);
  }

  // Append a new line
  _formatted_log_record.push_back('\n');
}
#else
/***/
template <typename... Args>
typename std::enable_if_t<!detail::any_is_same<std::wstring, void, Args...>::value, void> PatternFormatter::format(
  std::chrono::nanoseconds timestamp,
  const char* thread_id,
  char const* logger_name,
  detail::StaticLogRecordInfo const& logline_info,
  Args const&... args) const
{
  // clear out existing buffer
  _formatted_log_record.clear();

  // Format part 1 of the pattern first
  if (_pattern_formatter_helper_part_1)
  {
    _pattern_formatter_helper_part_1->format(_formatted_log_record, timestamp, thread_id, logger_name, logline_info);
  }

  // Format the user requested string
  fmt::format_to(_formatted_log_record, logline_info.message_format(), args...);

  // Format part 3 of the pattern
  if (_pattern_formatter_helper_part_3)
  {
    _pattern_formatter_helper_part_3->format(_formatted_log_record, timestamp, thread_id, logger_name, logline_info);
  }

  // Append a new line
  _formatted_log_record.push_back('\n');
}

/***/
template <typename... Args>
typename std::enable_if_t<detail::any_is_same<std::wstring, void, Args...>::value, void> PatternFormatter::format(
  std::chrono::nanoseconds timestamp,
  char const* thread_id,
  char const* logger_name,
  detail::StaticLogRecordInfo const& logline_info,
  Args const&... args) const
{
  // clear out existing buffer
  _formatted_log_record.clear();

  // Format part 1 of the pattern first
  if (_pattern_formatter_helper_part_1)
  {
    _pattern_formatter_helper_part_1->format(_formatted_log_record, timestamp, thread_id, logger_name, logline_info);
  }

  // Format the user message format string to a wide char buffer
  std::wstring const w_message_format = detail::s2ws(logline_info.message_format());

  // Format the whole message to a wide buffer
  _w_memory_buffer.clear();
  fmt::format_to(_w_memory_buffer, w_message_format, args...);

  // Convert the results to UTF-8
  detail::wstring_to_utf8(_w_memory_buffer, _formatted_log_record);

  // Format part 3 of the pattern
  if (_pattern_formatter_helper_part_3)
  {
    _pattern_formatter_helper_part_3->format(_formatted_log_record, timestamp, thread_id, logger_name, logline_info);
  }

  // Append a new line
  _formatted_log_record.push_back('\n');
}
#endif

/***/
template <typename TConstantString>
void PatternFormatter::_set_pattern(TConstantString)
{
  std::string const pattern = TConstantString::value();

  // parse and check the given pattern
  size_t const message_found = pattern.find("%(message)");
  if (message_found == std::string::npos)
  {
    throw std::runtime_error("%(message) is required in the format pattern");
  }

  // break down the pattern to three parts, we can ignore part_2 which will be '%(message)'
  std::string const pattern_part_1 = pattern.substr(0, message_found);
  std::string const pattern_part_3 = pattern.substr(message_found + quill::detail::strlen("%(message)"));

  // calculate the size of the format specifiers '%'
  // pos 0 = number of part_1 args, pos 1 = number of part_3 args
  constexpr std::array<size_t, 2> arr = _parse_format_pattern<TConstantString>();
  constexpr size_t part_1_args_count = arr[0];
  constexpr size_t part_3_args_count = arr[1];

  _pattern_formatter_helper_part_1 = std::move(_make_formatter_helper<part_1_args_count>(pattern_part_1));

  _pattern_formatter_helper_part_3 = std::move(_make_formatter_helper<part_3_args_count>(pattern_part_3));
}

/***/
template <typename TConstantString>
constexpr std::array<size_t, 2> PatternFormatter::_parse_format_pattern()
{
  constexpr char const* pattern = TConstantString::value();
  constexpr size_t len = quill::detail::strlen(pattern);

  size_t pos{0};
  bool part_2_found = false;

  size_t part_1_style_counter{0};
  size_t part_3_style_counter{0};

  while (pos < len - 1)
  {
    // TODO:: Handle error when e.g %[%(
    if (pattern[pos] == '%')
    {
      // if we haven't found message yet and we always expect it
      if (!part_2_found && pattern[++pos] == '(' && pattern[++pos] == 'm' && pattern[++pos] == 'e' &&
          pattern[++pos] == 's' && pattern[++pos] == 's' && pattern[++pos] == 'a' &&
          pattern[++pos] == 'g' && pattern[++pos] == 'e' && pattern[++pos] == ')')
      {
        // do not increment the style counter
        part_2_found = true;
      }
      else if (!part_2_found)
      {
        // we haven't found part_2 yet so we are still in part_1
        part_1_style_counter += 1;
      }
      else if (part_2_found)
      {
        // we found part_2 so now anything else is part 3
        part_3_style_counter += 1;
      }
    }
    pos += 1;
  }

  return std::array<size_t, 2>{{part_1_style_counter, part_3_style_counter}};
}

/***/
template <size_t N>
std::unique_ptr<PatternFormatter::FormatterHelperBase> PatternFormatter::_make_formatter_helper(std::string const& format_pattern_part)
{
  // Generate a vector of callbacks based on the specified format
  // We generate the appropriate callback in the same order and we also generate a fmt format string for the same pattern
  std::vector<argument_callback_t> const args_callback_collection =
    _generate_vector_of_callbacks(format_pattern_part);

  if (!args_callback_collection.empty())
  {
    // Wrap the vector of functions to a tuple for part 1
    auto const callbacks_tuple =
      _generate_tuple<N>([args_callback_collection](size_t i) { return args_callback_collection[i]; });

    std::string const fmt_format = _generate_fmt_format_string(format_pattern_part);

    // Store the tuple in a class for part 1
    return std::make_unique<FormatterHelper<decltype(callbacks_tuple)>>(callbacks_tuple, fmt_format);
  }
  return nullptr;
}

} // namespace quill