#ifndef CHROME_COMMON_CHROME_ISOLATED_WORLD_IDS_H_
#define CHROME_COMMON_CHROME_ISOLATED_WORLD_IDS_H_

#include "content/public/common/isolated_world_ids.h"

enum ChromeIsolatedWorldIDs {
  // Isolated world ID for Chrome Translate.
  ISOLATED_WORLD_ID_TRANSLATE = content::ISOLATED_WORLD_ID_CONTENT_END + 1,

  // Isolated world ID for internal Chrome features.
  ISOLATED_WORLD_ID_CHROME_INTERNAL,

  // Numbers for isolated worlds for extensions are set in
  // extensions/renderer/script_injection.cc, and are are greater than or equal
  // to this number.
  ISOLATED_WORLD_ID_EXTENSIONS
};

#endif
