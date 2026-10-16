/* test_ini.c -- host tests for the ini reader (ported from the KOTOR I loader).
 * Not built into the VPK.
 *
 * Build and run:
 *   gcc -std=c99 -Wall -Wextra -Werror -I loader \
 *       -o /tmp/test_ini tools/test_ini.c loader/ini.c \
 *   && /tmp/test_ini
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ini.h"

/* The shape swkotor2.ini has on the card: CRLF, a header comment, and
 * several sections the engine writes back whenever it saves options. */
static const char *kRealish =
  "; Star Wars Knights of the Old Republic II\r\n"
  "\r\n"
  "[Display Options]\r\n"
  "FullScreen=1\r\n"
  "Width=800\r\n"
  "\r\n"
  "[Game Options]\r\n"
  "Difficulty Level=1\r\n"
  "Language=fr\r\n"
  "Mouse Sensitivity=5\r\n"
  "\r\n"
  "[Sound Options]\r\n"
  "Disable Sound=0\r\n";

static void expect(const char *text, const char *section, const char *key,
                   int want_found, const char *want_value) {
  char got[64];
  int found = ini_get(text, section, key, got, sizeof got);
  if (found != want_found || strcmp(got, want_value) != 0) {
    printf("FAIL [%s] %s: found=%d \"%s\", wanted found=%d \"%s\"\n",
           section, key, found, got, want_found, want_value);
    assert(0);
  }
}

static void test_finds_key_in_its_section(void) {
  expect(kRealish, "Game Options", "Language", 1, "fr");
  expect(kRealish, "Sound Options", "Disable Sound", 1, "0");
  expect(kRealish, "Display Options", "Width", 1, "800");
}

static void test_missing_key_and_section(void) {
  expect(kRealish, "Game Options", "Nonsense", 0, "");
  expect(kRealish, "No Such Section", "Language", 0, "");
  expect("", "Game Options", "Language", 0, "");
}

/* The key exists, but under a different section -- the commonest way a hand
 * edit goes wrong, and it must read as absent rather than as a match. */
static void test_key_outside_its_section_is_not_found(void) {
  expect("[Sound Options]\nLanguage=fr\n", "Game Options", "Language", 0, "");
}

/* The engine's own reader is case-insensitive, and the file on the card is
 * users capitalise unpredictably. */
static void test_case_insensitive(void) {
  expect(kRealish, "game options", "language", 1, "fr");
  expect(kRealish, "GAME OPTIONS", "LANGUAGE", 1, "fr");
}

static void test_whitespace_is_stripped(void) {
  expect("[Game Options]\n  Language   =   de   \n", "Game Options",
         "Language", 1, "de");
  expect("  [Game Options]  \nLanguage=de\n", "Game Options", "Language", 1,
         "de");
}

static void test_comments_ignored(void) {
  expect("[Game Options]\n; Language=fr\nLanguage=it\n", "Game Options",
         "Language", 1, "it");
  expect("[Game Options]\n# Language=fr\nLanguage=it\n", "Game Options",
         "Language", 1, "it");
  expect("[Game Options]\nLanguage=it ; was fr\n", "Game Options", "Language",
         1, "it");
}

/* A key is only a key if the whole name matches: "Languages" must not answer a
 * request for "Language". */
static void test_prefix_is_not_a_match(void) {
  expect("[Game Options]\nLanguages=fr\n", "Game Options", "Language", 0, "");
  expect("[Game Options Extra]\nLanguage=fr\n", "Game Options", "Language", 0,
         "");
}

static void test_first_match_wins(void) {
  expect("[Game Options]\nLanguage=fr\nLanguage=de\n", "Game Options",
         "Language", 1, "fr");
}

static void test_empty_value(void) {
  expect("[Game Options]\nLanguage=\n", "Game Options", "Language", 1, "");
}

static void test_last_line_without_newline(void) {
  expect("[Game Options]\nLanguage=es", "Game Options", "Language", 1, "es");
}

/* A section runs until the next header, including across blank lines, and a
 * later repeat of the same header is still that section. */
static void test_section_repeated(void) {
  expect("[Game Options]\nFoo=1\n[Other]\nBar=2\n[Game Options]\nLanguage=de\n",
         "Game Options", "Language", 1, "de");
}

static void test_truncates_rather_than_overflows(void) {
  char got[4];
  int found = ini_get("[S]\nK=abcdefgh\n", "S", "K", got, sizeof got);
  assert(found == 1);
  assert(strcmp(got, "abc") == 0);
}

/* outsz 0 must not write anything at all -- there is nowhere to put the NUL. */
static void test_zero_sized_buffer(void) {
  char got[2] = { 'x', 'y' };
  (void)ini_get("[S]\nK=v\n", "S", "K", got, 0);
  assert(got[0] == 'x' && got[1] == 'y');
}

static void test_language_ids(void) {
  assert(ini_language_id("en") == INI_LANG_EN);
  assert(ini_language_id("fr") == INI_LANG_FR);
  assert(ini_language_id("it") == INI_LANG_IT);
  assert(ini_language_id("de") == INI_LANG_DE);
  assert(ini_language_id("es") == INI_LANG_ES);
}

/* The numbers getCurrentLanguage answers with: French 1, Italian 2, German 3,
 * Spanish 4, as the APK's ASPYR.getCurrentLanguage and libkotor2 have them (not
 * the talk tables' German 2, Italian 3). */
static void test_language_numbers_are_getcurrentlanguages(void) {
  assert(INI_LANG_FR == 1 && INI_LANG_IT == 2 && INI_LANG_DE == 3 && INI_LANG_ES == 4);
}

static void test_language_ids_case_insensitive(void) {
  assert(ini_language_id("FR") == INI_LANG_FR);
  assert(ini_language_id("De") == INI_LANG_DE);
}

/* Everything unrecognised is English, because the engine's own out-of-range
 * fallback is English. */
static void test_unknown_language_is_english(void) {
  assert(ini_language_id("") == INI_LANG_EN);
  assert(ini_language_id("xx") == INI_LANG_EN);
  assert(ini_language_id("french") == INI_LANG_EN);
  assert(ini_language_id("f") == INI_LANG_EN);
  assert(ini_language_id(NULL) == INI_LANG_EN);
}

/* Writing a language back out has to land on the code that reads it back as
 * the same id, or the picker would save a choice the next boot ignores. */
static void test_language_code_round_trips(void) {
  for (int id = INI_LANG_EN; id <= INI_LANG_ES; id++)
    assert(ini_language_id(ini_language_code(id)) == id);
  assert(strcmp(ini_language_code(INI_LANG_DE), "de") == 0);
}

static void test_language_code_out_of_range_is_english(void) {
  assert(strcmp(ini_language_code(-1), "en") == 0);
  assert(strcmp(ini_language_code(99), "en") == 0);
}

/* ---------------------------------------------------------------- ini_set */

/* ini_set rewrites the whole file, so every test states the exact bytes it
 * expects back: a writer that quietly reflows the user's file is a writer that
 * will one day eat a setting it did not understand. */
static void expect_set(const char *text, const char *section, const char *key,
                       const char *value, const char *want) {
  char out[1024];
  size_t need = ini_set(text, section, key, value, out, sizeof out);
  if (need != strlen(want) || strcmp(out, want) != 0) {
    printf("FAIL ini_set [%s] %s=%s\n  got  (%u) \"%s\"\n  want (%u) \"%s\"\n",
           section, key, value, (unsigned)need, out, (unsigned)strlen(want),
           want);
    assert(0);
  }
}

static void test_set_replaces_existing_value(void) {
  expect_set("[Game Options]\nLanguage=fr\n", "Game Options", "Language", "de",
             "[Game Options]\nLanguage=de\n");
}

/* The real file is CRLF with sections either side of ours; everything outside
 * the one line we touch must come back byte for byte. */
static void test_set_leaves_the_rest_of_the_file_alone(void) {
  expect_set(kRealish, "Game Options", "Language", "es",
             "; Star Wars Knights of the Old Republic II\r\n"
             "\r\n"
             "[Display Options]\r\n"
             "FullScreen=1\r\n"
             "Width=800\r\n"
             "\r\n"
             "[Game Options]\r\n"
             "Difficulty Level=1\r\n"
             "Language=es\r\n"
             "Mouse Sensitivity=5\r\n"
             "\r\n"
             "[Sound Options]\r\n"
             "Disable Sound=0\r\n");
}

/* The commonest case in the field: the engine wrote the file, so the section
 * is there but our key never has been. */
static void test_set_inserts_key_into_existing_section(void) {
  expect_set("[Game Options]\r\nDifficulty Level=1\r\n", "Game Options",
             "Language", "it",
             "[Game Options]\r\nLanguage=it\r\nDifficulty Level=1\r\n");
}

static void test_set_appends_missing_section(void) {
  expect_set("[Sound Options]\nDisable Sound=0\n", "Game Options", "Language",
             "fr",
             "[Sound Options]\nDisable Sound=0\n[Game Options]\nLanguage=fr\n");
}

/* No file at all -- a fresh card. CRLF, because that is what the engine writes
 * and the file is its to own from then on. */
static void test_set_on_empty_text_writes_a_whole_file(void) {
  expect_set("", "Game Options", "Language", "de",
             "[Game Options]\r\nLanguage=de\r\n");
}

/* A file whose last line has no newline must not get the new section welded
 * onto the end of it. */
static void test_set_terminates_last_line_before_appending(void) {
  expect_set("[Sound Options]\nDisable Sound=0", "Game Options", "Language",
             "es",
             "[Sound Options]\nDisable Sound=0\n[Game Options]\nLanguage=es\n");
}

/* Whatever the user typed to the left of the '=' is theirs: spelling,
 * indentation and spacing all survive a rewrite. */
static void test_set_preserves_how_the_key_was_written(void) {
  expect_set("[Game Options]\n  LANGUAGE  = fr\n", "Game Options", "Language",
             "de", "[Game Options]\n  LANGUAGE  = de\n");
}

/* An inline comment describes the old value, so carrying it over would leave
 * the file actively lying. It goes. */
static void test_set_drops_a_stale_inline_comment(void) {
  expect_set("[Game Options]\nLanguage=fr ; was de\n", "Game Options",
             "Language", "it", "[Game Options]\nLanguage=it\n");
}

/* A commented-out key is not the key: the live one below it is what changes. */
static void test_set_skips_a_commented_out_key(void) {
  expect_set("[Game Options]\n; Language=fr\nLanguage=de\n", "Game Options",
             "Language", "es",
             "[Game Options]\n; Language=fr\nLanguage=es\n");
}

static void test_set_ignores_same_key_in_another_section(void) {
  expect_set("[Sound Options]\nLanguage=fr\n[Game Options]\nFoo=1\n",
             "Game Options", "Language", "de",
             "[Sound Options]\nLanguage=fr\n[Game Options]\nLanguage=de\nFoo=1\n");
}

/* ini_get answers with the first match, so ini_set must change that same one
 * or the write would appear to have done nothing. */
static void test_set_replaces_the_one_ini_get_would_read(void) {
  expect_set("[Game Options]\nLanguage=fr\nLanguage=it\n", "Game Options",
             "Language", "de",
             "[Game Options]\nLanguage=de\nLanguage=it\n");
}

static void test_set_finds_key_in_a_repeated_section(void) {
  expect_set("[Game Options]\nFoo=1\n[Other]\nBar=2\n[Game Options]\nLanguage=fr\n",
             "Game Options", "Language", "de",
             "[Game Options]\nFoo=1\n[Other]\nBar=2\n[Game Options]\nLanguage=de\n");
}

static void test_set_is_case_insensitive(void) {
  expect_set("[GAME OPTIONS]\nlanguage=fr\n", "Game Options", "Language", "de",
             "[GAME OPTIONS]\nlanguage=de\n");
}

/* The return is the length the result needs, so a caller that guessed its
 * buffer too small can tell -- and must not write the truncated text to the
 * card. */
static void test_set_reports_the_length_it_needs(void) {
  const char *text = "[Game Options]\nLanguage=fr\n";
  char out[8];
  size_t need = ini_set(text, "Game Options", "Language", "de", out, sizeof out);
  assert(need == strlen("[Game Options]\nLanguage=de\n"));
  assert(need >= sizeof out);                 /* truncated: do not write it */
  assert(strlen(out) == sizeof out - 1);      /* still NUL-terminated */
  assert(strncmp(out, "[Game Op", 7) == 0);
}

static void test_set_sizing_call_writes_nothing(void) {
  char out[2] = { 'x', 'y' };
  size_t need = ini_set("[S]\nK=v\n", "S", "K", "w", out, 0);
  assert(need == strlen("[S]\nK=w\n"));
  assert(out[0] == 'x' && out[1] == 'y');
}

/* The two halves have to agree: whatever ini_set writes, ini_get must read
 * back. This is the property the picker actually depends on. */
static void test_set_then_get_round_trips(void) {
  static const char *const kStarts[] = {
    "", "[Game Options]\r\nLanguage=fr\r\n", "[Sound Options]\nDisable Sound=0\n",
    "[Game Options]\r\nDifficulty Level=1\r\n",
  };
  static const char *const kCodes[] = { "en", "fr", "de", "it", "es" };
  for (size_t i = 0; i < sizeof kStarts / sizeof kStarts[0]; i++) {
    for (size_t j = 0; j < sizeof kCodes / sizeof kCodes[0]; j++) {
      char out[1024], got[16];
      size_t need = ini_set(kStarts[i], "Game Options", "Language", kCodes[j],
                            out, sizeof out);
      assert(need < sizeof out);
      assert(ini_get(out, "Game Options", "Language", got, sizeof got) == 1);
      assert(strcmp(got, kCodes[j]) == 0);
    }
  }
}

int main(void) {
  test_finds_key_in_its_section();
  test_missing_key_and_section();
  test_key_outside_its_section_is_not_found();
  test_case_insensitive();
  test_whitespace_is_stripped();
  test_comments_ignored();
  test_prefix_is_not_a_match();
  test_first_match_wins();
  test_empty_value();
  test_last_line_without_newline();
  test_section_repeated();
  test_truncates_rather_than_overflows();
  test_zero_sized_buffer();
  test_language_ids();
  test_language_numbers_are_getcurrentlanguages();
  test_language_ids_case_insensitive();
  test_unknown_language_is_english();
  test_language_code_round_trips();
  test_language_code_out_of_range_is_english();
  test_set_replaces_existing_value();
  test_set_leaves_the_rest_of_the_file_alone();
  test_set_inserts_key_into_existing_section();
  test_set_appends_missing_section();
  test_set_on_empty_text_writes_a_whole_file();
  test_set_terminates_last_line_before_appending();
  test_set_preserves_how_the_key_was_written();
  test_set_drops_a_stale_inline_comment();
  test_set_skips_a_commented_out_key();
  test_set_ignores_same_key_in_another_section();
  test_set_replaces_the_one_ini_get_would_read();
  test_set_finds_key_in_a_repeated_section();
  test_set_is_case_insensitive();
  test_set_reports_the_length_it_needs();
  test_set_sizing_call_writes_nothing();
  test_set_then_get_round_trips();
  printf("test_ini: all tests passed\n");
  return 0;
}
