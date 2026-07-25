#include "enrol_url.h"

#include <ctype.h>
#include <string.h>

namespace {

// Copies at most size-1 bytes and always terminates.
bool copy_bounded(char* out, size_t size, const char* begin, size_t length) {
  if (length >= size) {
    return false;
  }
  memcpy(out, begin, length);
  out[length] = '\0';
  return true;
}

const char* skip_leading_space(const char* text) {
  while (*text != '\0' && isspace(static_cast<unsigned char>(*text))) {
    ++text;
  }
  return text;
}

size_t length_without_trailing_space(const char* text) {
  size_t length = strlen(text);
  while (length > 0 && isspace(static_cast<unsigned char>(text[length - 1]))) {
    --length;
  }
  return length;
}

}  // namespace

EnrolUrl enrol_url_parse(const char* text) {
  EnrolUrl result;
  if (text == nullptr) {
    return result;
  }

  // Pasting from a browser or a chat message routinely brings whitespace along.
  text = skip_leading_space(text);
  const size_t total = length_without_trailing_space(text);
  if (total == 0) {
    return result;
  }

  size_t scheme_length = 0;
  if (strncasecmp(text, "https://", 8) == 0) {
    scheme_length = 8;
  } else if (strncasecmp(text, "http://", 7) == 0) {
    scheme_length = 7;
  } else {
    // No scheme means we would have to guess between http and https, and
    // guessing wrong on the VPS silently sends the token in clear.
    return result;
  }

  // The authority runs to the first '/', '?' or '#' after the scheme.
  size_t authority_end = scheme_length;
  while (authority_end < total && text[authority_end] != '/' && text[authority_end] != '?' &&
         text[authority_end] != '#') {
    ++authority_end;
  }
  if (authority_end == scheme_length) {
    return result;  // scheme but no host
  }

  if (!copy_bounded(result.base, sizeof(result.base), text, authority_end)) {
    return result;
  }

  // The token, if the URL carries one. Looked for anywhere in the query rather
  // than assuming it is the first parameter.
  const char* query = nullptr;
  for (size_t i = authority_end; i < total; ++i) {
    if (text[i] == '?') {
      query = text + i + 1;
      break;
    }
  }

  if (query != nullptr) {
    const char* end_of_query = text + total;
    const char* cursor = query;
    while (cursor < end_of_query) {
      const char* pair_end = cursor;
      while (pair_end < end_of_query && *pair_end != '&' && *pair_end != '#') {
        ++pair_end;
      }
      if (static_cast<size_t>(pair_end - cursor) > 6 && strncasecmp(cursor, "token=", 6) == 0) {
        const char* value = cursor + 6;
        if (!copy_bounded(result.token, sizeof(result.token), value,
                          static_cast<size_t>(pair_end - value))) {
          return result;  // a token that long is not one of ours
        }
        break;
      }
      cursor = pair_end + 1;
    }
  }

  result.ok = true;
  return result;
}
