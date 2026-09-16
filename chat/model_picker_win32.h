#ifndef DARKCHAT_MODEL_PICKER_WIN32_H
#define DARKCHAT_MODEL_PICKER_WIN32_H

/* Searchable model picker. The popup is a thin Win32 shell over a snapshot of
   the merged catalog; selection is retained by id and re-resolved against the
   newest snapshot, never by an index into a source that has been replaced.
   The modal pump is a separate entry point so the host suite can substitute a
   deterministic pump and exercise open/cancel/accept/refresh without blocking. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdbool.h>
#include "model_catalog.h"

typedef struct ModelPicker ModelPicker;

/* Creates the popup (hidden) and copies `source`. The filter starts empty, so
   the full merged list is shown, and the list selection is initialized to
   `initial_id` when it is present. */
ModelPicker *model_picker_create(HWND owner, const ChatModelCatalog *source,
    const wchar_t *status, const wchar_t *initial_id);
/* Runs the modal loop until accept, cancel, or close. */
void model_picker_pump(ModelPicker *picker);
/* Replaces the source snapshot and status, preserving the filter text and the
   selected id (nearest index when the id disappeared). */
void model_picker_source_updated(ModelPicker *picker,
    const ChatModelCatalog *source, const wchar_t *status);

bool model_picker_accepted(const ModelPicker *picker);
const wchar_t *model_picker_selected_id(const ModelPicker *picker);

/* Terminal actions; safe outside the pump as well. */
void model_picker_accept(ModelPicker *picker);
void model_picker_cancel(ModelPicker *picker);

/* Deterministic seams for the host/controller suite. */
void model_picker_set_filter(ModelPicker *picker, const wchar_t *filter);
void model_picker_set_selected(ModelPicker *picker, const wchar_t *id);
const wchar_t *model_picker_filter(const ModelPicker *picker);
size_t model_picker_match_count(const ModelPicker *picker);
const wchar_t *model_picker_match_id(const ModelPicker *picker, size_t index);

void model_picker_destroy(ModelPicker *picker);

#endif
