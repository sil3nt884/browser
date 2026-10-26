// Copyright
#include "chrome/app/chrome_main_delegate.h"

#include <stddef.h>

#include "base/command_line.h"

std::optional<int> ChromeMainDelegate::BasicStartupComplete() {
#if BUILDFLAG(IS_WIN)
  // Foo.
#endif
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  chrome::RegisterPathProvider();
  return std::nullopt;
}
