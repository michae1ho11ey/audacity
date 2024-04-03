/*!********************************************************************
 
 Audacity: A Digital Audio Editor
 
 @file DarkThemeAsCeeCode.cpp
 
 Paul Licameli split from Theme.cpp
 
 **********************************************************************/

#include <vector>
#include "Theme.h"

static const std::vector<unsigned char> ImageCacheAsData {
// Include the generated file full of numbers
#include "DarkModernThemeAsCeeCode.h"
};

static ThemeBase::RegisteredTheme theme{
   { "dark-modern", XO("DarkModern") }, PreferredSystemAppearance::Dark, ImageCacheAsData
};
