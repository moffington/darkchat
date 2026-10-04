#ifndef DARKCHAT_EXPORT_PACKAGE_H
#define DARKCHAT_EXPORT_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

/* Filesystem-name and reference-string rules for the Markdown attachment
   package: <name>.md beside <name>.attachments\/<64-hex>.<ext>.

   Pure; no Win32, no file I/O. These rules live apart from both serializers
   and from the host so that the name an export writes and the name an import
   accepts are decided in one place and can be tested without a filesystem.
   When they disagree, the failure mode is an export a user cannot import. */

/* Longest package folder component, in UTF-8 BYTES, exclusive. The importer
   validates a `file` reference's folder as a bounded UTF-8 token, so this is
   the bound that actually has to hold, and it is the one the exporter obeys.
   Measuring bytes rather than UTF-16 units is what keeps a stem containing
   non-ASCII from being accepted here and rejected there. */
#define CHAT_EXPORT_SIDECAR_FOLDER_MAX 80
/* Suffix every package folder carries, in bytes (12, no terminator). */
#define CHAT_EXPORT_SIDECAR_SUFFIX ".attachments"
/* The same suffix as a wide literal, for building a path with swprintf. */
#define CHAT_EXPORT_SIDECAR_SUFFIX_W L".attachments"
/* The stem limit is DERIVED from the folder bound, not chosen independently:
   "<stem>.attachments" must fit inside it. Two limits chosen separately would
   let this module accept a package its own importer rejects. */
#define CHAT_EXPORT_STEM_MAX \
    (CHAT_EXPORT_SIDECAR_FOLDER_MAX - 12 - 1)

/* The extension a stored MIME is named with in a sidecar filename.
   "image/png"->"png", "image/jpeg"->"jpg", "image/webp"->"webp",
   "image/gif"->"gif". `out` receives at most 8 units.

   False for any other MIME. A sidecar must be nameable, so an unmapped MIME
   fails the export rather than inventing an extension: guessing wrong would
   produce a package whose files no reader recognises. Only these four are
   mapped because they are the only four an ingest can produce. */
bool chat_export_sidecar_extension(const char *mime, char out[8]);

/* True when name[0..length) begins with a reserved Win32 DOS device
   basename. Windows refuses such a name however it is extended, so a caller
   constructing a filename must prefix it. Case-insensitive, and stops at the
   first dot: "CON", "CON.txt" and "con.anything" are reserved, while
   "CONSOLE" and "COM10" are not.

   Exported so chat_export_default_name and the stem helper below cannot
   drift apart: a package name the save dialog could never suggest must also
   be one the stem helper refuses. */
bool chat_filename_is_reserved_device(const wchar_t *name, size_t length);

/* Derives the package stem from the full path the user chose in the save
   dialog.

   Splits at the last '/' or '\' first and applies every rule to the
   BASENAME only: a drive colon and directory separators in the directory
   part are legal and must never be rejected, which is why this takes a path
   and not a bare name. Then, on the basename: strip a trailing ".md" or
   ".markdown" case-insensitively; fail on an empty result, on "." or "..",
   on a character Windows forbids in a filename (< > : " | ? * and controls),
   on a reserved device basename, and when the result's UTF-8 length exceeds
   CHAT_EXPORT_STEM_MAX, which is what keeps "<stem>.attachments" inside the
   folder bound the importer enforces.
   `out` is written only on success and is always
   NUL-terminated. */
bool chat_export_sidecar_stem_from_path(const wchar_t *path,
    wchar_t *out, size_t capacity);

/* Validates an import's `file` reference and splits it into its two
   components. `out_folder` and `out_basename` each receive at most 80 units
   and are written only on success.

   Accepted shape, exactly:
     "<folder>/<basename>" with exactly one '/', where
       - neither component is empty, "." or ".."
       - neither contains '\', ':' or a control character
       - neither contains a character Windows forbids in a filename
         (< > " | ? *)
       - <folder> ends in ".attachments"
       - <basename> is exactly 64 lowercase hex characters, '.', and one of
         the extensions chat_export_sidecar_extension knows

   Each component is then a bounded token containing no separator, so a
   caller can join them under the .md's own directory and the result cannot
   escape it. That is what makes the check sufficient: there is deliberately
   no ".." substring test, because "draft..final" is a legal filename and
   rejecting it would make the user's own export unimportable. The folder is
   not required to match the .md's stem, so a package survives being renamed
   as long as its folder stays beside it; correctness does not rest on the
   folder, because the importer re-hashes the bytes it reads and compares
   them against the digest the document itself declares. */
bool chat_import_sidecar_reference(const char *file, size_t file_length,
    char out_folder[80], char out_basename[80]);

/* Percent-encodes a sidecar path for a Markdown image destination, so the
   characters Windows allows in a filename but that still carry URL meaning
   ('#', '%', '?' and space) cannot alter the link. The unreserved set
   A-Z a-z 0-9 - . _ ~ plus the '/' separator is emitted literally; every
   other unit is written as its UTF-8 bytes in %XX form with uppercase hex.

   `path` is the combined "<folder>/<basename>" reference. Writes at most
   capacity-1 units and always terminates; false when the encoded form would
   not fit or when a surrogate pair would be split. */
bool chat_export_markdown_destination(const wchar_t *path,
    wchar_t *out, size_t capacity);

/* Escapes a display name for a Markdown image alt text. Backslash, '[' and
   ']' are backslash-escaped and CR/LF collapse to a single space, so a name
   can never open a link label or inject a second one. Writes at most
   capacity-1 units, always terminates, and never splits a surrogate pair. */
bool chat_export_markdown_alt(const wchar_t *display_name,
    wchar_t *out, size_t capacity);

#endif