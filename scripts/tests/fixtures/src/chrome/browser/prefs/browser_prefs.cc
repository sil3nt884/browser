#include "chrome/browser/prefs/browser_prefs.h"

#include "base/values.h"

void RegisterUserProfilePrefs(user_prefs::PrefRegistrySyncable* registry,
                              const std::string& locale) {
  RegisterProfilePrefs(registry, locale);

#if BUILDFLAG(IS_ANDROID)
  ::android::RegisterUserProfilePrefs(registry);
#endif
}
