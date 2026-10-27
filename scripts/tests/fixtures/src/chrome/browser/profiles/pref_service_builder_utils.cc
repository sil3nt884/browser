// Copyright 2014 The Chromium Authors
#include "chrome/browser/profiles/pref_service_builder_utils.h"

#include <stddef.h>

void RegisterProfilePrefs(bool is_signin_profile,
                          const std::string& locale,
                          user_prefs::PrefRegistrySyncable* pref_registry) {
#if BUILDFLAG(IS_CHROMEOS)
  if (is_signin_profile)
    RegisterSigninProfilePrefs(pref_registry, GetCountry());
  else
#endif
    RegisterUserProfilePrefs(pref_registry, locale);

  SimpleDependencyManager::GetInstance()->RegisterProfilePrefsForServices(
      pref_registry);
  BrowserContextDependencyManager::GetInstance()
      ->RegisterProfilePrefsForServices(pref_registry);
}
