/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FontFaceSetImpl.h"

#include "gfxFontConstants.h"
#include "gfxFontSrcPrincipal.h"
#include "gfxFontSrcURI.h"
#include "gfxFontUtils.h"
#include "mozilla/css/Loader.h"
#include "mozilla/dom/CSSFontFaceRule.h"
#include "mozilla/dom/DocumentInlines.h"
#include "mozilla/dom/Event.h"
#include "mozilla/dom/FontFaceImpl.h"
#include "mozilla/dom/FontFaceSet.h"
#include "mozilla/dom/FontFaceSetBinding.h"
#include "mozilla/dom/FontFaceSetLoadEvent.h"
#include "mozilla/dom/FontFaceSetLoadEventBinding.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/WorkerCommon.h"
#include "mozilla/FontPropertyTypes.h"
#include "mozilla/AsyncEventDispatcher.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "mozilla/PresShell.h"
#include "mozilla/PresShellInlines.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/ServoCSSParser.h"
#include "mozilla/ServoStyleSet.h"
#include "mozilla/ServoUtils.h"
#include "mozilla/Sprintf.h"
#include "mozilla/StaticPrefs_layout.h"
#include "mozilla/Telemetry.h"
#include "mozilla/LoadInfo.h"
#include "nsComponentManagerUtils.h"
#include "nsContentUtils.h"
#include "nsDeviceContext.h"
#include "nsFontFaceLoader.h"
#include "nsIConsoleService.h"
#include "nsIContentPolicy.h"
#include "nsIDocShell.h"
#include "nsILoadContext.h"
#include "nsIPrincipal.h"
#include "nsIWebNavigation.h"
#include "nsNetUtil.h"
#include "nsIInputStream.h"
#include "nsLayoutUtils.h"
#include "nsPresContext.h"
#include "nsPrintfCString.h"
#include "nsUTF8Utils.h"
#include "nsDOMNavigationTiming.h"
#include "ReferrerInfo.h"

using namespace mozilla;
using namespace mozilla::css;
using namespace mozilla::dom;

#define LOG(args) \
  MOZ_LOG(gfxUserFontSet::GetUserFontsLog(), mozilla::LogLevel::Debug, args)
#define LOG_ENABLED() \
  MOZ_LOG_TEST(gfxUserFontSet::GetUserFontsLog(), LogLevel::Debug)

NS_IMPL_ISUPPORTS0(FontFaceSetImpl)

FontFaceSetImpl::FontFaceSetImpl(FontFaceSet* aOwner)
    : mMutex("mozilla::dom::FontFaceSetImpl"),
      mOwner(aOwner),
      mStatus(FontFaceSetLoadStatus::Loaded),
      mNonRuleFacesDirty(false),
      mHasLoadingFontFaces(false),
      mHasLoadingFontFacesIsDirty(false),
      mDelayedLoadCheck(false),
      mBypassCache(false),
      mPrivateBrowsing(false) {}

FontFaceSetImpl::~FontFaceSetImpl() {
  // Assert that we don't drop any FontFaceSet objects during a Servo traversal,
  // since PostTraversalTask objects can hold raw pointers to FontFaceSets.
  MOZ_ASSERT(!gfxFontUtils::IsInServoTraversal());

  Destroy();
}

void FontFaceSetImpl::Destroy() {
  RecursiveMutexAutoLock lock(mMutex);

  for (const auto& key : mLoaders.Keys()) {
    key->Cancel();
  }

  mLoaders.Clear();
  mNonRuleFaces.Clear();
  gfxUserFontSet::Destroy();
  mOwner = nullptr;
}

void FontFaceSetImpl::ParseFontShorthandForMatching(
    const nsACString& aFont, StyleFontFamilyList& aFamilyList,
    FontWeight& aWeight, FontStretch& aStretch, FontSlantStyle& aStyle,
    ErrorResult& aRv) {
  RefPtr<URLExtraData> url = GetURLExtraData();
  if (!url) {
    aRv.ThrowInvalidStateError("Missing URLExtraData");
    return;
  }

  if (!ServoCSSParser::ParseFontShorthandForMatching(
          aFont, url, aFamilyList, aStyle, aStretch, aWeight)) {
    aRv.ThrowSyntaxError("Invalid font shorthand");
    return;
  }
}

static bool HasAnyCharacterInUnicodeRange(gfxUserFontEntry* aEntry,
                                          const nsAString& aInput) {
  const char16_t* p = aInput.Data();
  const char16_t* end = p + aInput.Length();

  while (p < end) {
    uint32_t c = UTF16CharEnumerator::NextChar(&p, end);
    if (aEntry->CharacterInUnicodeRange(c)) {
      return true;
    }
  }
  return false;
}

void FontFaceSetImpl::FindMatchingFontFaces(const nsACString& aFont,
                                            const nsAString& aText,
                                            nsTArray<FontFace*>& aFontFaces,
                                            ErrorResult& aRv) {
  RecursiveMutexAutoLock lock(mMutex);

  StyleFontFamilyList familyList;
  FontWeight weight;
  FontStretch stretch;
  FontSlantStyle italicStyle;
  ParseFontShorthandForMatching(aFont, familyList, weight, stretch, italicStyle,
                                aRv);
  if (aRv.Failed()) {
    return;
  }

  gfxFontStyle style;
  style.style = italicStyle;
  style.weight = weight;
  style.stretch = stretch;

  // Set of FontFaces that we want to return.
  nsTHashSet<FontFace*> matchingFaces;

  for (const StyleSingleFontFamily& fontFamilyName : familyList.list.AsSpan()) {
    if (!fontFamilyName.IsFamilyName()) {
      continue;
    }

    const auto& name = fontFamilyName.AsFamilyName();
    RefPtr<gfxFontFamily> family =
        LookupFamily(nsAtomCString(name.name.AsAtom()));

    if (!family) {
      continue;
    }

    AutoTArray<gfxFontEntry*, 4> entries;
    family->FindAllFontsForStyle(style, entries);

    for (gfxFontEntry* e : entries) {
      FontFaceImpl::Entry* entry = static_cast<FontFaceImpl::Entry*>(e);
      if (HasAnyCharacterInUnicodeRange(entry, aText)) {
        entry->FindFontFaceOwners(matchingFaces);
      }
    }
  }

  if (matchingFaces.IsEmpty()) {
    return;
  }

  // Add all FontFaces in matchingFaces to aFontFaces, in the order
  // they appear in the FontFaceSet.
  FindMatchingFontFaces(matchingFaces, aFontFaces);
}

void FontFaceSetImpl::FindMatchingFontFaces(
    const nsTHashSet<FontFace*>& aMatchingFaces,
    nsTArray<FontFace*>& aFontFaces) {
  RecursiveMutexAutoLock lock(mMutex);
  for (FontFaceRecord& record : mNonRuleFaces) {
    FontFace* owner = record.mFontFace->GetOwner();
    if (owner && aMatchingFaces.Contains(owner)) {
      aFontFaces.AppendElement(owner);
    }
  }
}

bool FontFaceSetImpl::ReadyPromiseIsPending() const {
  RecursiveMutexAutoLock lock(mMutex);
  return mOwner && mOwner->ReadyPromiseIsPending();
}

FontFaceSetLoadStatus FontFaceSetImpl::Status() {
  RecursiveMutexAutoLock lock(mMutex);
  FlushUserFontSet();
  return mStatus;
}

bool FontFaceSetImpl::Add(FontFaceImpl* aFontFace, ErrorResult& aRv) {
  RecursiveMutexAutoLock lock(mMutex);
  FlushUserFontSet();

  if (aFontFace->IsInFontFaceSet(this)) {
    return false;
  }

  if (aFontFace->HasRule()) {
    aRv.ThrowInvalidModificationError(
        "Can't add face to FontFaceSet that comes from an @font-face rule");
    return false;
  }

  aFontFace->AddFontFaceSet(this);

#ifdef DEBUG
  for (const FontFaceRecord& rec : mNonRuleFaces) {
    MOZ_ASSERT(rec.mFontFace != aFontFace,
               "FontFace should not occur in mNonRuleFaces twice");
  }
#endif

  FontFaceRecord* rec = mNonRuleFaces.AppendElement();
  rec->mFontFace = aFontFace;
  rec->mOrigin = Nothing();

  mNonRuleFacesDirty = true;
  MarkUserFontSetDirty();
  mHasLoadingFontFacesIsDirty = true;
  CheckLoadingStarted();
  return true;
}

void FontFaceSetImpl::Clear() {
  RecursiveMutexAutoLock lock(mMutex);
  FlushUserFontSet();

  if (mNonRuleFaces.IsEmpty()) {
    return;
  }

  for (size_t i = 0; i < mNonRuleFaces.Length(); i++) {
    FontFaceImpl* f = mNonRuleFaces[i].mFontFace;
    f->RemoveFontFaceSet(this);
  }

  mNonRuleFaces.Clear();
  mNonRuleFacesDirty = true;
  MarkUserFontSetDirty();
  mHasLoadingFontFacesIsDirty = true;
  CheckLoadingFinished();
}

bool FontFaceSetImpl::Delete(FontFaceImpl* aFontFace) {
  RecursiveMutexAutoLock lock(mMutex);
  FlushUserFontSet();

  if (aFontFace->HasRule()) {
    return false;
  }

  bool removed = false;
  for (size_t i = 0; i < mNonRuleFaces.Length(); i++) {
    if (mNonRuleFaces[i].mFontFace == aFontFace) {
      mNonRuleFaces.RemoveElementAt(i);
      removed = true;
      break;
    }
  }
  if (!removed) {
    return false;
  }

  aFontFace->RemoveFontFaceSet(this);

  mNonRuleFacesDirty = true;
  MarkUserFontSetDirty();
  mHasLoadingFontFacesIsDirty = true;
  CheckLoadingFinished();
  return true;
}

bool FontFaceSetImpl::HasAvailableFontFace(FontFaceImpl* aFontFace) {
  return aFontFace->IsInFontFaceSet(this);
}

void FontFaceSetImpl::RemoveLoader(nsFontFaceLoader* aLoader) {
  RecursiveMutexAutoLock lock(mMutex);
  mLoaders.RemoveEntry(aLoader);
}

void FontFaceSetImpl::InsertNonRuleFontFace(FontFaceImpl* aFontFace,
                                            bool& aFontSetModified) {
  nsAtom* fontFamily = aFontFace->GetFamilyName();
  if (!fontFamily) {
    // If there is no family name, this rule cannot contribute a
    // usable font, so there is no point in processing it further.
    return;
  }

  nsAtomCString family(fontFamily);

  // Just create a new font entry if we haven't got one already.
  if (!aFontFace->GetUserFontEntry()) {
    // XXX Should we be checking mLocalRulesUsed like InsertRuleFontFace does?
    RefPtr<gfxUserFontEntry> entry = FindOrCreateUserFontEntryFromFontFace(
        family, aFontFace, StyleOrigin::Author);
    if (!entry) {
      return;
    }
    aFontFace->SetUserFontEntry(entry);
  }

  aFontSetModified = true;
  AddUserFontEntry(family, aFontFace->GetUserFontEntry());
}

/* static */
already_AddRefed<gfxUserFontEntry>
FontFaceSetImpl::FindOrCreateUserFontEntryFromFontFace(
    FontFaceImpl* aFontFace) {
  nsAtom* fontFamily = aFontFace->GetFamilyName();
  if (!fontFamily) {
    // If there is no family name, this rule cannot contribute a
    // usable font, so there is no point in processing it further.
    return nullptr;
  }

  return FindOrCreateUserFontEntryFromFontFace(nsAtomCString(fontFamily),
                                               aFontFace, StyleOrigin::Author);
}

static WeightRange GetWeightRangeForDescriptor(
    const Maybe<StyleComputedFontWeightRange>& aVal,
    gfxFontEntry::RangeFlags& aRangeFlags) {
  if (!aVal) {
    aRangeFlags |= gfxFontEntry::RangeFlags::eAutoWeight;
    return WeightRange(FontWeight::NORMAL);
  }
  return WeightRange(FontWeight::FromFloat(aVal->_0),
                     FontWeight::FromFloat(aVal->_1));
}

static SlantStyleRange GetStyleRangeForDescriptor(
    const Maybe<StyleComputedFontStyleDescriptor>& aVal,
    gfxFontEntry::RangeFlags& aRangeFlags) {
  if (!aVal) {
    aRangeFlags |= gfxFontEntry::RangeFlags::eAutoSlantStyle;
    return SlantStyleRange(FontSlantStyle::NORMAL);
  }
  auto& val = *aVal;
  switch (val.tag) {
    case StyleComputedFontStyleDescriptor::Tag::Normal:
      return SlantStyleRange(FontSlantStyle::NORMAL);
    case StyleComputedFontStyleDescriptor::Tag::Italic:
      return SlantStyleRange(FontSlantStyle::ITALIC);
    case StyleComputedFontStyleDescriptor::Tag::Oblique:
      return SlantStyleRange(FontSlantStyle::FromFloat(val.AsOblique()._0),
                             FontSlantStyle::FromFloat(val.AsOblique()._1));
  }
  MOZ_ASSERT_UNREACHABLE("How?");
  return SlantStyleRange(FontSlantStyle::NORMAL);
}

static StretchRange GetStretchRangeForDescriptor(
    const Maybe<StyleComputedFontStretchRange>& aVal,
    gfxFontEntry::RangeFlags& aRangeFlags) {
  if (!aVal) {
    aRangeFlags |= gfxFontEntry::RangeFlags::eAutoStretch;
    return StretchRange(FontStretch::NORMAL);
  }
  return StretchRange(aVal->_0, aVal->_1);
}

// TODO(emilio): Should this take an nsAtom* aFamilyName instead?
//
// All callers have one handy.
/* static */
already_AddRefed<gfxUserFontEntry>
FontFaceSetImpl::FindOrCreateUserFontEntryFromFontFace(
    const nsACString& aFamilyName, FontFaceImpl* aFontFace,
    StyleOrigin aOrigin) {
  FontFaceSetImpl* set = aFontFace->GetPrimaryFontFaceSet();

  uint32_t languageOverride = NO_FONT_LANGUAGE_OVERRIDE;
  StyleFontDisplay fontDisplay = StyleFontDisplay::Auto;
  float ascentOverride = -1.0;
  float descentOverride = -1.0;
  float lineGapOverride = -1.0;
  float sizeAdjust = 1.0;

  gfxFontEntry::RangeFlags rangeFlags = gfxFontEntry::RangeFlags::eNoFlags;

  // set up weight
  WeightRange weight =
      GetWeightRangeForDescriptor(aFontFace->GetFontWeight(), rangeFlags);

  // set up stretch
  StretchRange stretch =
      GetStretchRangeForDescriptor(aFontFace->GetFontStretch(), rangeFlags);

  // set up font style
  SlantStyleRange italicStyle =
      GetStyleRangeForDescriptor(aFontFace->GetFontStyle(), rangeFlags);

  // set up font display
  if (Maybe<StyleFontDisplay> display = aFontFace->GetFontDisplay()) {
    fontDisplay = *display;
  }

  // set up font metrics overrides
  if (Maybe<StylePercentage> ascent = aFontFace->GetAscentOverride()) {
    ascentOverride = ascent->_0;
  }
  if (Maybe<StylePercentage> descent = aFontFace->GetDescentOverride()) {
    descentOverride = descent->_0;
  }
  if (Maybe<StylePercentage> lineGap = aFontFace->GetLineGapOverride()) {
    lineGapOverride = lineGap->_0;
  }

  // set up size-adjust scaling factor
  if (Maybe<StylePercentage> percentage = aFontFace->GetSizeAdjust()) {
    sizeAdjust = percentage->_0;
  }

  // set up font features
  nsTArray<gfxFontFeature> featureSettings;
  aFontFace->GetFontFeatureSettings(featureSettings);

  // set up font variations
  nsTArray<gfxFontVariation> variationSettings;
  aFontFace->GetFontVariationSettings(variationSettings);

  // set up font language override
  if (Maybe<StyleFontLanguageOverride> descriptor =
          aFontFace->GetFontLanguageOverride()) {
    languageOverride = descriptor->_0;
  }

  // set up unicode-range
  gfxCharacterMap* unicodeRanges = aFontFace->GetUnicodeRangeAsCharacterMap();

  RefPtr<gfxUserFontEntry> existingEntry = aFontFace->GetUserFontEntry();
  if (existingEntry) {
    // aFontFace already has a user font entry, so we update its attributes
    // rather than creating a new one.
    existingEntry->UpdateAttributes(
        weight, stretch, italicStyle, featureSettings, variationSettings,
        languageOverride, unicodeRanges, fontDisplay, rangeFlags,
        ascentOverride, descentOverride, lineGapOverride, sizeAdjust);
    // If the family name has changed, remove the entry from its current family
    // and clear the mFamilyName field so it can be reset when added to a new
    // family.
    if (!existingEntry->mFamilyName.IsEmpty() &&
        existingEntry->mFamilyName != aFamilyName) {
      gfxUserFontFamily* family = set->LookupFamily(existingEntry->mFamilyName);
      if (family) {
        family->RemoveFontEntry(existingEntry);
      }
      existingEntry->mFamilyName.Truncate(0);
    }
    return existingEntry.forget();
  }

  // set up src array
  nsTArray<gfxFontFaceSrc> srcArray;

  if (aFontFace->HasFontData()) {
    gfxFontFaceSrc* face = srcArray.AppendElement();
    if (!face) {
      return nullptr;
    }

    face->mSourceType = gfxFontFaceSrc::eSourceType_Buffer;
    face->mBuffer = aFontFace->TakeBufferSource();
  } else {
    AutoTArray<StyleFontFaceSourceListComponent, 8> sourceListComponents;
    aFontFace->GetSources(sourceListComponents);
    size_t len = sourceListComponents.Length();
    for (size_t i = 0; i < len; ++i) {
      gfxFontFaceSrc* face = srcArray.AppendElement();
      const auto& component = sourceListComponents[i];
      switch (component.tag) {
        case StyleFontFaceSourceListComponent::Tag::Local: {
          nsAtom* atom = component.AsLocal();
          face->mLocalName.Append(nsAtomCString(atom));
          face->mSourceType = gfxFontFaceSrc::eSourceType_Local;
          face->mURI = nullptr;
          face->mFormatHint = StyleFontFaceSourceFormatKeyword::None;
          break;
        }

        case StyleFontFaceSourceListComponent::Tag::Url: {
          face->mSourceType = gfxFontFaceSrc::eSourceType_URL;
          const StyleCssUrl* url = component.AsUrl();
          nsIURI* uri = url->GetURI();
          face->mURI = uri ? new gfxFontSrcURI(uri) : nullptr;
          const URLExtraData& extraData = url->ExtraData();
          face->mReferrerInfo = extraData.ReferrerInfo();

          // agent and user stylesheets are treated slightly differently,
          // the same-site origin check and access control headers are
          // enforced against the sheet principal rather than the document
          // principal to allow user stylesheets to include @font-face rules
          if (aOrigin == StyleOrigin::User ||
              aOrigin == StyleOrigin::UserAgent) {
            face->mUseOriginPrincipal = true;
            face->mOriginPrincipal = new gfxFontSrcPrincipal(
                extraData.Principal(), extraData.Principal());
          }

          face->mLocalName.Truncate();
          face->mFormatHint = StyleFontFaceSourceFormatKeyword::None;
          face->mTechFlags = StyleFontFaceSourceTechFlags::Empty();

          if (i + 1 < len) {
            // Check for a format hint.
            const auto& next = sourceListComponents[i + 1];
            switch (next.tag) {
              case StyleFontFaceSourceListComponent::Tag::FormatHintKeyword:
                face->mFormatHint = next.format_hint_keyword._0;
                i++;
                break;
              case StyleFontFaceSourceListComponent::Tag::FormatHintString: {
                nsDependentCSubstring valueString(
                    reinterpret_cast<const char*>(
                        next.format_hint_string.utf8_bytes),
                    next.format_hint_string.length);

                if (valueString.LowerCaseEqualsASCII("woff")) {
                  face->mFormatHint = StyleFontFaceSourceFormatKeyword::Woff;
                } else if (valueString.LowerCaseEqualsASCII("woff2")) {
                  face->mFormatHint = StyleFontFaceSourceFormatKeyword::Woff2;
                } else if (valueString.LowerCaseEqualsASCII("opentype")) {
                  face->mFormatHint =
                      StyleFontFaceSourceFormatKeyword::Opentype;
                } else if (valueString.LowerCaseEqualsASCII("truetype")) {
                  face->mFormatHint =
                      StyleFontFaceSourceFormatKeyword::Truetype;
                } else if (valueString.LowerCaseEqualsASCII("truetype-aat")) {
                  face->mFormatHint =
                      StyleFontFaceSourceFormatKeyword::Truetype;
                } else if (valueString.LowerCaseEqualsASCII(
                               "embedded-opentype")) {
                  face->mFormatHint =
                      StyleFontFaceSourceFormatKeyword::EmbeddedOpentype;
                } else if (valueString.LowerCaseEqualsASCII("svg")) {
                  face->mFormatHint = StyleFontFaceSourceFormatKeyword::Svg;
                } else if (StaticPrefs::layout_css_font_variations_enabled()) {
                  // Non-standard values that Firefox accepted, for back-compat;
                  // these are superseded by the tech() function.
                  if (valueString.LowerCaseEqualsASCII("woff-variations")) {
                    face->mFormatHint = StyleFontFaceSourceFormatKeyword::Woff;
                  } else if (valueString.LowerCaseEqualsASCII(
                                 "woff2-variations")) {
                    face->mFormatHint = StyleFontFaceSourceFormatKeyword::Woff2;
                  } else if (valueString.LowerCaseEqualsASCII(
                                 "opentype-variations")) {
                    face->mFormatHint =
                        StyleFontFaceSourceFormatKeyword::Opentype;
                  } else if (valueString.LowerCaseEqualsASCII(
                                 "truetype-variations")) {
                    face->mFormatHint =
                        StyleFontFaceSourceFormatKeyword::Truetype;
                  } else {
                    face->mFormatHint =
                        StyleFontFaceSourceFormatKeyword::Unknown;
                  }
                } else {
                  // unknown format specified, mark to distinguish from the
                  // case where no format hints are specified
                  face->mFormatHint = StyleFontFaceSourceFormatKeyword::Unknown;
                }
                i++;
                break;
              }
              case StyleFontFaceSourceListComponent::Tag::TechFlags:
              case StyleFontFaceSourceListComponent::Tag::Local:
              case StyleFontFaceSourceListComponent::Tag::Url:
                break;
            }
          }

          if (i + 1 < len) {
            // Check for a set of font-technologies flags.
            const auto& next = sourceListComponents[i + 1];
            if (next.IsTechFlags()) {
              face->mTechFlags = next.AsTechFlags();
              i++;
            }
          }

          if (!face->mURI) {
            // if URI not valid, omit from src array
            srcArray.RemoveLastElement();
            NS_WARNING("null url in @font-face rule");
            continue;
          }
          break;
        }

        case StyleFontFaceSourceListComponent::Tag::FormatHintKeyword:
        case StyleFontFaceSourceListComponent::Tag::FormatHintString:
        case StyleFontFaceSourceListComponent::Tag::TechFlags:
          MOZ_ASSERT_UNREACHABLE(
              "Should always come after a URL source, and be consumed already");
          break;
      }
    }
  }

  if (srcArray.IsEmpty()) {
    return nullptr;
  }

  RefPtr<gfxUserFontEntry> entry = set->FindOrCreateUserFontEntry(
      aFamilyName, srcArray, weight, stretch, italicStyle, featureSettings,
      variationSettings, languageOverride, unicodeRanges, fontDisplay,
      rangeFlags, ascentOverride, descentOverride, lineGapOverride, sizeAdjust);

  return entry.forget();
}

nsresult FontFaceSetImpl::LogMessage(gfxUserFontEntry* aUserFontEntry,
                                     uint32_t aSrcIndex, const char* aMessage,
                                     uint32_t aFlags, nsresult aStatus) {
  nsAutoCString familyName;
  nsAutoCString fontURI;
  aUserFontEntry->GetFamilyNameAndURIForLogging(aSrcIndex, familyName, fontURI);

  nsAutoCString weightString;
  aUserFontEntry->Weight().ToString(weightString);
  nsAutoCString stretchString;
  aUserFontEntry->Stretch().ToString(stretchString);
  nsPrintfCString message(
      "downloadable font: %s "
      "(font-family: \"%s\" style:%s weight:%s stretch:%s src index:%d)",
      aMessage, familyName.get(),
      aUserFontEntry->IsItalic() ? "italic" : "normal",  // XXX todo: oblique?
      weightString.get(), stretchString.get(), aSrcIndex);

  if (NS_FAILED(aStatus)) {
    message.AppendLiteral(": ");
    switch (aStatus) {
      case NS_ERROR_DOM_BAD_URI:
        message.AppendLiteral("bad URI or cross-site access not allowed");
        break;
      case NS_ERROR_CONTENT_BLOCKED:
        message.AppendLiteral("content blocked");
        break;
      default:
        message.AppendLiteral("status=");
        message.AppendInt(static_cast<uint32_t>(aStatus));
        break;
    }
  }
  message.AppendLiteral(" source: ");
  message.Append(fontURI);

  LOG(("userfonts (%p) %s", this, message.get()));

  if (GetCurrentThreadWorkerPrivate()) {
    // TODO(aosmond): Log to the console for workers. See bug 1778537.
    return NS_OK;
  }

  nsCOMPtr<nsIConsoleService> console(
      do_GetService(NS_CONSOLESERVICE_CONTRACTID));
  if (!console) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // try to give the user an indication of where the rule came from
  RawServoFontFaceRule* rule = FindRuleForUserFontEntry(aUserFontEntry);
  nsString href;
  nsAutoCString text;
  uint32_t line = 0;
  uint32_t column = 0;
  if (rule) {
    Servo_FontFaceRule_GetCssText(rule, &text);
    Servo_FontFaceRule_GetSourceLocation(rule, &line, &column);
    // FIXME We need to figure out an approach to get the style sheet
    // of this raw rule. See bug 1450903.
#if 0
    StyleSheet* sheet = rule->GetStyleSheet();
    // if the style sheet is removed while the font is loading can be null
    if (sheet) {
      nsCString spec = sheet->GetSheetURI()->GetSpecOrDefault();
      CopyUTF8toUTF16(spec, href);
    } else {
      NS_WARNING("null parent stylesheet for @font-face rule");
      href.AssignLiteral("unknown");
    }
#endif
    // Leave href empty if we don't know how to get the correct sheet.
  }

  nsresult rv;
  nsCOMPtr<nsIScriptError> scriptError =
      do_CreateInstance(NS_SCRIPTERROR_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = scriptError->InitWithWindowID(NS_ConvertUTF8toUTF16(message),
                                     href,                         // file
                                     NS_ConvertUTF8toUTF16(text),  // src line
                                     line, column,
                                     aFlags,        // flags
                                     "CSS Loader",  // category (make separate?)
                                     GetInnerWindowID());
  if (NS_SUCCEEDED(rv)) {
    console->LogMessage(scriptError);
  }

  return NS_OK;
}

nsresult FontFaceSetImpl::SyncLoadFontData(gfxUserFontEntry* aFontToLoad,
                                           const gfxFontFaceSrc* aFontFaceSrc,
                                           uint8_t*& aBuffer,
                                           uint32_t& aBufferLength) {
  nsCOMPtr<nsIChannel> channel;
  nsresult rv = CreateChannelForSyncLoadFontData(getter_AddRefs(channel),
                                                 aFontToLoad, aFontFaceSrc);
  NS_ENSURE_SUCCESS(rv, rv);

  // blocking stream is OK for data URIs
  nsCOMPtr<nsIInputStream> stream;
  rv = channel->Open(getter_AddRefs(stream));
  NS_ENSURE_SUCCESS(rv, rv);

  uint64_t bufferLength64;
  rv = stream->Available(&bufferLength64);
  NS_ENSURE_SUCCESS(rv, rv);
  if (bufferLength64 == 0) {
    return NS_ERROR_FAILURE;
  }
  if (bufferLength64 > UINT32_MAX) {
    return NS_ERROR_FILE_TOO_BIG;
  }
  aBufferLength = static_cast<uint32_t>(bufferLength64);

  // read all the decoded data
  aBuffer = static_cast<uint8_t*>(malloc(sizeof(uint8_t) * aBufferLength));
  if (!aBuffer) {
    aBufferLength = 0;
    return NS_ERROR_OUT_OF_MEMORY;
  }

  uint32_t numRead, totalRead = 0;
  while (NS_SUCCEEDED(
             rv = stream->Read(reinterpret_cast<char*>(aBuffer + totalRead),
                               aBufferLength - totalRead, &numRead)) &&
         numRead != 0) {
    totalRead += numRead;
    if (totalRead > aBufferLength) {
      rv = NS_ERROR_FAILURE;
      break;
    }
  }

  // make sure there's a mime type
  if (NS_SUCCEEDED(rv)) {
    nsAutoCString mimeType;
    rv = channel->GetContentType(mimeType);
    aBufferLength = totalRead;
  }

  if (NS_FAILED(rv)) {
    free(aBuffer);
    aBuffer = nullptr;
    aBufferLength = 0;
    return rv;
  }

  return NS_OK;
}

void FontFaceSetImpl::OnFontFaceStatusChanged(FontFaceImpl* aFontFace) {
  gfxFontUtils::AssertSafeThreadOrServoFontMetricsLocked();
  RecursiveMutexAutoLock lock(mMutex);
  MOZ_ASSERT(HasAvailableFontFace(aFontFace));

  mHasLoadingFontFacesIsDirty = true;

  if (aFontFace->Status() == FontFaceLoadStatus::Loading) {
    CheckLoadingStarted();
  } else {
    MOZ_ASSERT(aFontFace->Status() == FontFaceLoadStatus::Loaded ||
               aFontFace->Status() == FontFaceLoadStatus::Error);
    // When a font finishes downloading, nsPresContext::UserFontSetUpdated
    // will be called immediately afterwards to request a reflow of the
    // relevant elements in the document.  We want to wait until the reflow
    // request has been done before the FontFaceSet is marked as Loaded so
    // that we don't briefly set the FontFaceSet to Loaded and then Loading
    // again once the reflow is pending.  So we go around the event loop
    // and call CheckLoadingFinished() after the reflow has been queued.
    if (!mDelayedLoadCheck) {
      mDelayedLoadCheck = true;
      DispatchCheckLoadingFinishedAfterDelay();
    }
  }
}

void FontFaceSetImpl::DispatchCheckLoadingFinishedAfterDelay() {
  gfxFontUtils::AssertSafeThreadOrServoFontMetricsLocked();

  if (ServoStyleSet* set = gfxFontUtils::CurrentServoStyleSet()) {
    // See comments in Gecko_GetFontMetrics.
    //
    // We can't just dispatch the runnable below if we're not on the main
    // thread, since it needs to take a strong reference to the FontFaceSet,
    // and being a DOM object, FontFaceSet doesn't support thread-safe
    // refcounting.
    set->AppendTask(
        PostTraversalTask::DispatchFontFaceSetCheckLoadingFinishedAfterDelay(
            this));
    return;
  }

  DispatchToOwningThread(
      "FontFaceSetImpl::DispatchCheckLoadingFinishedAfterDelay",
      [self = RefPtr{this}]() { self->CheckLoadingFinishedAfterDelay(); });
}

void FontFaceSetImpl::CheckLoadingFinishedAfterDelay() {
  RecursiveMutexAutoLock lock(mMutex);
  mDelayedLoadCheck = false;
  CheckLoadingFinished();
}

void FontFaceSetImpl::CheckLoadingStarted() {
  gfxFontUtils::AssertSafeThreadOrServoFontMetricsLocked();
  RecursiveMutexAutoLock lock(mMutex);

  if (!HasLoadingFontFaces()) {
    return;
  }

  if (mStatus == FontFaceSetLoadStatus::Loading) {
    // We have already dispatched a loading event and replaced mReady
    // with a fresh, unresolved promise.
    return;
  }

  mStatus = FontFaceSetLoadStatus::Loading;

  if (IsOnOwningThread()) {
    OnLoadingStarted();
    return;
  }

  DispatchToOwningThread("FontFaceSetImpl::CheckLoadingStarted",
                         [self = RefPtr{this}]() { self->OnLoadingStarted(); });
}

void FontFaceSetImpl::OnLoadingStarted() {
  RecursiveMutexAutoLock lock(mMutex);
  if (mOwner) {
    mOwner->DispatchLoadingEventAndReplaceReadyPromise();
  }
}

void FontFaceSetImpl::UpdateHasLoadingFontFaces() {
  RecursiveMutexAutoLock lock(mMutex);
  mHasLoadingFontFacesIsDirty = false;
  mHasLoadingFontFaces = false;
  for (size_t i = 0; i < mNonRuleFaces.Length(); i++) {
    if (mNonRuleFaces[i].mFontFace->Status() == FontFaceLoadStatus::Loading) {
      mHasLoadingFontFaces = true;
      return;
    }
  }
}

bool FontFaceSetImpl::HasLoadingFontFaces() {
  RecursiveMutexAutoLock lock(mMutex);
  if (mHasLoadingFontFacesIsDirty) {
    UpdateHasLoadingFontFaces();
  }
  return mHasLoadingFontFaces;
}

bool FontFaceSetImpl::MightHavePendingFontLoads() {
  // Check for FontFace objects in the FontFaceSet that are still loading.
  return HasLoadingFontFaces();
}

void FontFaceSetImpl::CheckLoadingFinished() {
  RecursiveMutexAutoLock lock(mMutex);
  if (mDelayedLoadCheck) {
    // Wait until the runnable posted in OnFontFaceStatusChanged calls us.
    return;
  }

  if (!ReadyPromiseIsPending()) {
    // We've already resolved mReady (or set the flag to do that lazily) and
    // dispatched the loadingdone/loadingerror events.
    return;
  }

  if (MightHavePendingFontLoads()) {
    // We're not finished loading yet.
    return;
  }

  mStatus = FontFaceSetLoadStatus::Loaded;

  if (IsOnOwningThread()) {
    OnLoadingFinished();
    return;
  }

  DispatchToOwningThread(
      "FontFaceSetImpl::CheckLoadingFinished",
      [self = RefPtr{this}]() { self->OnLoadingFinished(); });
}

void FontFaceSetImpl::OnLoadingFinished() {
  RecursiveMutexAutoLock lock(mMutex);
  if (mOwner) {
    mOwner->MaybeResolve();
  }
}

void FontFaceSetImpl::RefreshStandardFontLoadPrincipal() {
  RecursiveMutexAutoLock lock(mMutex);
  mAllowedFontLoads.Clear();
  IncrementGeneration(false);
}

// -- gfxUserFontSet
// ------------------------------------------------

already_AddRefed<gfxFontSrcPrincipal>
FontFaceSetImpl::GetStandardFontLoadPrincipal() const {
  RecursiveMutexAutoLock lock(mMutex);
  return RefPtr{mStandardFontLoadPrincipal}.forget();
}

void FontFaceSetImpl::RecordFontLoadDone(uint32_t aFontSize,
                                         TimeStamp aDoneTime) {
  mDownloadCount++;
  mDownloadSize += aFontSize;
  Telemetry::Accumulate(Telemetry::WEBFONT_SIZE, aFontSize / 1024);

  TimeStamp navStart = GetNavigationStartTimeStamp();
  TimeStamp zero;
  if (navStart != zero) {
    Telemetry::AccumulateTimeDelta(Telemetry::WEBFONT_DOWNLOAD_TIME_AFTER_START,
                                   navStart, aDoneTime);
  }
}

void FontFaceSetImpl::DoRebuildUserFontSet() { MarkUserFontSetDirty(); }

already_AddRefed<gfxUserFontEntry> FontFaceSetImpl::CreateUserFontEntry(
    const nsTArray<gfxFontFaceSrc>& aFontFaceSrcList, WeightRange aWeight,
    StretchRange aStretch, SlantStyleRange aStyle,
    const nsTArray<gfxFontFeature>& aFeatureSettings,
    const nsTArray<gfxFontVariation>& aVariationSettings,
    uint32_t aLanguageOverride, gfxCharacterMap* aUnicodeRanges,
    StyleFontDisplay aFontDisplay, RangeFlags aRangeFlags,
    float aAscentOverride, float aDescentOverride, float aLineGapOverride,
    float aSizeAdjust) {
  RefPtr<gfxUserFontEntry> entry = new FontFaceImpl::Entry(
      this, aFontFaceSrcList, aWeight, aStretch, aStyle, aFeatureSettings,
      aVariationSettings, aLanguageOverride, aUnicodeRanges, aFontDisplay,
      aRangeFlags, aAscentOverride, aDescentOverride, aLineGapOverride,
      aSizeAdjust);
  return entry.forget();
}

#undef LOG_ENABLED
#undef LOG
