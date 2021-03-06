/*
*******************************************************************************
* Copyright (C) 2011, International Business Machines Corporation and         *
* others. All Rights Reserved.                                                *
*******************************************************************************
*/

#include "unicode/utypes.h"

#if !UCONFIG_NO_FORMATTING

#include "tzgnames.h"

#include "unicode/basictz.h"
#include "unicode/locdspnm.h"
#include "unicode/msgfmt.h"
#include "unicode/rbtz.h"
#include "unicode/simpletz.h"
#include "unicode/vtzone.h"

#include "cmemory.h"
#include "cstring.h"
#include "uhash.h"
#include "uassert.h"
#include "umutex.h"
#include "uresimp.h"
#include "ureslocs.h"
#include "zonemeta.h"
#include "tznames_impl.h"
#include "olsontz.h"

U_NAMESPACE_BEGIN

#define ZID_KEY_MAX  128

static const char gZoneStrings[]                = "zoneStrings";

static const char gRegionFormatTag[]            = "regionFormat";
static const char gFallbackRegionFormatTag[]    = "fallbackRegionFormat";
static const char gFallbackFormatTag[]          = "fallbackFormat";

static const UChar gEmpty[]                     = {0x00};

static const UChar gDefRegionPattern[]          = {0x7B, 0x30, 0x7D, 0x00}; // "{0}"
static const UChar gDefFallbackRegionPattern[]  = {0x7B, 0x31, 0x7D, 0x20, 0x28, 0x7B, 0x30, 0x7D, 0x29, 0x00}; // "{1} ({0})"
static const UChar gDefFallbackPattern[]        = {0x7B, 0x31, 0x7D, 0x20, 0x28, 0x7B, 0x30, 0x7D, 0x29, 0x00}; // "{1} ({0})"

static const double kDstCheckRange      = (double)184*U_MILLIS_PER_DAY;



U_CDECL_BEGIN

typedef struct PartialLocationKey {
    const UChar* tzID;
    const UChar* mzID;
    UBool isLong;
} PartialLocationKey;

/**
 * Hash function for partial location name hash key
 */
static int32_t U_CALLCONV
hashPartialLocationKey(const UHashTok key) {
    // <tzID>&<mzID>#[L|S]
    PartialLocationKey *p = (PartialLocationKey *)key.pointer;
    UnicodeString str(p->tzID);
    str.append((UChar)0x26)
        .append(p->mzID)
        .append((UChar)0x23)
        .append((UChar)(p->isLong ? 0x4C : 0x53));
    return uhash_hashUCharsN(str.getBuffer(), str.length());
}

/**
 * Comparer for partial location name hash key
 */
static UBool U_CALLCONV
comparePartialLocationKey(const UHashTok key1, const UHashTok key2) {
    PartialLocationKey *p1 = (PartialLocationKey *)key1.pointer;
    PartialLocationKey *p2 = (PartialLocationKey *)key2.pointer;

    if (p1 == p2) {
        return TRUE;
    }
    if (p1 == NULL || p2 == NULL) {
        return FALSE;
    }
    // We just check identity of tzID/mzID
    return (p1->tzID == p2->tzID && p1->mzID == p2->mzID && p1->isLong == p2->isLong);
}

/**
 * Deleter for GNameInfo
 */
static void U_CALLCONV
deleteGNameInfo(void *obj) {
    uprv_free(obj);
}

/**
 * GNameInfo stores zone name information in the local trie
 */
typedef struct GNameInfo {
    UTimeZoneGenericNameType    type;
    const UChar*                tzID;
} ZNameInfo;

/**
 * GMatchInfo stores zone name match information used by find method
 */
typedef struct GMatchInfo {
    const GNameInfo*    gnameInfo;
    int32_t             matchLength;
    UTimeZoneTimeType   timeType;
} ZMatchInfo;

U_CDECL_END

// ---------------------------------------------------
// The class stores time zone generic name match information
// ---------------------------------------------------
TimeZoneGenericNameMatchInfo::TimeZoneGenericNameMatchInfo(UVector* matches)
: fMatches(matches) {
}

TimeZoneGenericNameMatchInfo::~TimeZoneGenericNameMatchInfo() {
    if (fMatches != NULL) {
        delete fMatches;
    }
}

int32_t
TimeZoneGenericNameMatchInfo::size() const {
    if (fMatches == NULL) {
        return 0;
    }
    return fMatches->size();
}

UTimeZoneGenericNameType
TimeZoneGenericNameMatchInfo::getGenericNameType(int32_t index) const {
    GMatchInfo *minfo = (GMatchInfo *)fMatches->elementAt(index);
    if (minfo != NULL) {
        return static_cast<UTimeZoneGenericNameType>(minfo->gnameInfo->type);
    }
    return UTZGNM_UNKNOWN;
}

int32_t
TimeZoneGenericNameMatchInfo::getMatchLength(int32_t index) const {
    ZMatchInfo *minfo = (ZMatchInfo *)fMatches->elementAt(index);
    if (minfo != NULL) {
        return minfo->matchLength;
    }
    return -1;
}

UnicodeString&
TimeZoneGenericNameMatchInfo::getTimeZoneID(int32_t index, UnicodeString& tzID) const {
    GMatchInfo *minfo = (GMatchInfo *)fMatches->elementAt(index);
    if (minfo != NULL && minfo->gnameInfo->tzID != NULL) {
        tzID.setTo(TRUE, minfo->gnameInfo->tzID, -1);
    } else {
        tzID.setToBogus();
    }
    return tzID;
}

// ---------------------------------------------------
// GNameSearchHandler
// ---------------------------------------------------
class GNameSearchHandler : public TextTrieMapSearchResultHandler {
public:
    GNameSearchHandler(uint32_t types);
    virtual ~GNameSearchHandler();

    UBool handleMatch(int32_t matchLength, const CharacterNode *node, UErrorCode &status);
    UVector* getMatches(int32_t& maxMatchLen);

private:
    uint32_t fTypes;
    UVector* fResults;
    int32_t fMaxMatchLen;
};

GNameSearchHandler::GNameSearchHandler(uint32_t types)
: fTypes(types), fResults(NULL), fMaxMatchLen(0) {
}

GNameSearchHandler::~GNameSearchHandler() {
    if (fResults != NULL) {
        delete fResults;
    }
}

UBool
GNameSearchHandler::handleMatch(int32_t matchLength, const CharacterNode *node, UErrorCode &status) {
    if (U_FAILURE(status)) {
        return FALSE;
    }
    if (node->hasValues()) {
        int32_t valuesCount = node->countValues();
        for (int32_t i = 0; i < valuesCount; i++) {
            GNameInfo *nameinfo = (ZNameInfo *)node->getValue(i);
            if (nameinfo == NULL) {
                break;
            }
            if ((nameinfo->type & fTypes) != 0) {
                // matches a requested type
                if (fResults == NULL) {
                    fResults = new UVector(uhash_freeBlock, NULL, status);
                    if (fResults == NULL) {
                        status = U_MEMORY_ALLOCATION_ERROR;
                    }
                }
                if (U_SUCCESS(status)) {
                    GMatchInfo *gmatch = (GMatchInfo *)uprv_malloc(sizeof(GMatchInfo));
                    if (gmatch == NULL) {
                        status = U_MEMORY_ALLOCATION_ERROR;
                    } else {
                        // add the match to the vector
                        gmatch->gnameInfo = nameinfo;
                        gmatch->matchLength = matchLength;
                        gmatch->timeType = UTZFMT_TIME_TYPE_UNKNOWN;
                        fResults->addElement(gmatch, status);
                        if (U_FAILURE(status)) {
                            uprv_free(gmatch);
                        } else {
                            if (matchLength > fMaxMatchLen) {
                                fMaxMatchLen = matchLength;
                            }
                        }
                    }
                }
            }
        }
    }
    return TRUE;
}

UVector*
GNameSearchHandler::getMatches(int32_t& maxMatchLen) {
    // give the ownership to the caller
    UVector *results = fResults;
    maxMatchLen = fMaxMatchLen;

    // reset
    fResults = NULL;
    fMaxMatchLen = 0;
    return results;
}

// ---------------------------------------------------
// TimeZoneGenericNames
//
// TimeZoneGenericNames is parallel to TimeZoneNames,
// but handles run-time generated time zone names.
// This is the main part of this module.
// ---------------------------------------------------
TimeZoneGenericNames::TimeZoneGenericNames(const Locale& locale, UErrorCode& status)
: fLocale(locale),
  fLock(NULL),
  fTimeZoneNames(NULL),
  fLocationNamesMap(NULL),
  fPartialLocationNamesMap(NULL),
  fRegionFormat(NULL),
  fFallbackRegionFormat(NULL),
  fFallbackFormat(NULL),
  fLocaleDisplayNames(NULL),
  fStringPool(status),
  fGNamesTrie(TRUE, deleteGNameInfo),
  fGNamesTrieFullyLoaded(FALSE) {
    initialize(locale, status);
}

TimeZoneGenericNames::~TimeZoneGenericNames() {
    cleanup();
    umtx_destroy(&fLock);
}

void
TimeZoneGenericNames::initialize(const Locale& locale, UErrorCode& status) {
    if (U_FAILURE(status)) {
        return;
    }

    // TimeZoneNames
    fTimeZoneNames = TimeZoneNames::createInstance(locale, status);
    if (U_FAILURE(status)) {
        return;
    }

    // Initialize format patterns
    UnicodeString rpat(TRUE, gDefRegionPattern, -1);
    UnicodeString frpat(TRUE, gDefFallbackRegionPattern, -1);
    UnicodeString fpat(TRUE, gDefFallbackPattern, -1);

    UErrorCode tmpsts = U_ZERO_ERROR;   // OK with fallback warning..
    UResourceBundle *zoneStrings = ures_open(U_ICUDATA_ZONE, locale.getName(), &tmpsts);
    zoneStrings = ures_getByKeyWithFallback(zoneStrings, gZoneStrings, zoneStrings, &tmpsts);

    if (U_SUCCESS(tmpsts)) {
        const UChar *regionPattern = ures_getStringByKeyWithFallback(zoneStrings, gRegionFormatTag, NULL, &tmpsts);
        if (U_SUCCESS(tmpsts) && u_strlen(regionPattern) > 0) {
            rpat.setTo(regionPattern);
        }
        tmpsts = U_ZERO_ERROR;
        const UChar *fallbackRegionPattern = ures_getStringByKeyWithFallback(zoneStrings, gFallbackRegionFormatTag, NULL, &tmpsts);
        if (U_SUCCESS(tmpsts) && u_strlen(fallbackRegionPattern) > 0) {
            frpat.setTo(fallbackRegionPattern);
        }
        tmpsts = U_ZERO_ERROR;
        const UChar *fallbackPattern = ures_getStringByKeyWithFallback(zoneStrings, gFallbackFormatTag, NULL, &tmpsts);
        if (U_SUCCESS(tmpsts) && u_strlen(fallbackPattern) > 0) {
            fpat.setTo(fallbackPattern);
        }
    }
    ures_close(zoneStrings);

    fRegionFormat = new MessageFormat(rpat, status);
    if (fRegionFormat == NULL) {
        status = U_MEMORY_ALLOCATION_ERROR;
    }
    fFallbackRegionFormat = new MessageFormat(frpat, status);
    if (fFallbackRegionFormat == NULL) {
        status = U_MEMORY_ALLOCATION_ERROR;
    }
    fFallbackFormat = new MessageFormat(fpat, status);
    if (fFallbackFormat == NULL) {
        status = U_MEMORY_ALLOCATION_ERROR;
    }
    if (U_FAILURE(status)) {
        cleanup();
        return;
    }

    // locale display names
    fLocaleDisplayNames = LocaleDisplayNames::createInstance(locale);

    // hash table for names - no key/value deleters
    fLocationNamesMap = uhash_open(uhash_hashUChars, uhash_compareUChars, NULL, &status);
    if (U_FAILURE(status)) {
        cleanup();
        return;
    }

    fPartialLocationNamesMap = uhash_open(hashPartialLocationKey, comparePartialLocationKey, NULL, &status);
    if (U_FAILURE(status)) {
        cleanup();
        return;
    }
    uhash_setKeyDeleter(fPartialLocationNamesMap, uhash_freeBlock);
    // no value deleter

    // target region
    const char* region = fLocale.getCountry();
    int32_t regionLen = uprv_strlen(region);
    if (regionLen == 0) {
        char loc[ULOC_FULLNAME_CAPACITY];
        uloc_addLikelySubtags(fLocale.getName(), loc, sizeof(loc), &status);

        regionLen = uloc_getCountry(loc, fTargetRegion, sizeof(fTargetRegion), &status);
        if (U_SUCCESS(status)) {
            fTargetRegion[regionLen] = 0;
        } else {
            cleanup();
            return;
        }
    } else if (regionLen < (int32_t)sizeof(fTargetRegion)) {
        uprv_strcpy(fTargetRegion, region);
    } else {
        fTargetRegion[0] = 0;
    }

    // preload generic names for the default zone
    TimeZone *tz = TimeZone::createDefault();
    const UChar *tzID = ZoneMeta::getCanonicalCLDRID(*tz);
    if (tzID != NULL) {
        loadStrings(UnicodeString(tzID));
    }
    delete tz;
}

void
TimeZoneGenericNames::cleanup() {
    if (fRegionFormat != NULL) {
        delete fRegionFormat;
    }
    if (fFallbackRegionFormat != NULL) {
        delete fFallbackRegionFormat;
    }
    if (fFallbackFormat != NULL) {
        delete fFallbackFormat;
    }
    if (fLocaleDisplayNames != NULL) {
        delete fLocaleDisplayNames;
    }
    if (fTimeZoneNames != NULL) {
        delete fTimeZoneNames;
    }

    uhash_close(fLocationNamesMap);
    uhash_close(fPartialLocationNamesMap);
}

UnicodeString&
TimeZoneGenericNames::getDisplayName(const TimeZone& tz, UTimeZoneGenericNameType type, UDate date, UnicodeString& name) const {
    name.setToBogus();
    switch (type) {
    case UTZGNM_LOCATION:
        {
            const UChar* tzCanonicalID = ZoneMeta::getCanonicalCLDRID(tz);
            if (tzCanonicalID != NULL) {
                getGenericLocationName(UnicodeString(tzCanonicalID), name);
            }
        }
        break;
    case UTZGNM_LONG:
    case UTZGNM_SHORT:
        formatGenericNonLocationName(tz, type, date, name);
        if (name.isEmpty()) {
            const UChar* tzCanonicalID = ZoneMeta::getCanonicalCLDRID(tz);
            if (tzCanonicalID != NULL) {
                getGenericLocationName(UnicodeString(tzCanonicalID), name);
            }
        }
        break;
    default:
        break;
    }
    return name;
}

UnicodeString&
TimeZoneGenericNames::getGenericLocationName(const UnicodeString& tzCanonicalID, UnicodeString& name) const {
    if (tzCanonicalID.isEmpty()) {
        name.setToBogus();
        return name;
    }

    const UChar *locname = NULL;
    TimeZoneGenericNames *nonConstThis = const_cast<TimeZoneGenericNames *>(this);
    umtx_lock(&nonConstThis->fLock);
    {
        locname = nonConstThis->getGenericLocationName(tzCanonicalID);
    }
    umtx_unlock(&nonConstThis->fLock);

    if (locname == NULL) {
        name.setToBogus();
    } else {
        name.setTo(TRUE, locname, -1);
    }

    return name;
}

/*
 * This method updates the cache and must be called with a lock
 */
const UChar*
TimeZoneGenericNames::getGenericLocationName(const UnicodeString& tzCanonicalID) {
    U_ASSERT(!tzCanonicalID.isEmpty());
    if (tzCanonicalID.length() > ZID_KEY_MAX) {
        return NULL;
    }

    UErrorCode status = U_ZERO_ERROR;
    UChar tzIDKey[ZID_KEY_MAX + 1];
    int32_t tzIDKeyLen = tzCanonicalID.extract(tzIDKey, ZID_KEY_MAX + 1, status);
    U_ASSERT(status == U_ZERO_ERROR);   // already checked length above
    tzIDKey[tzIDKeyLen] = 0;

    const UChar *locname = (const UChar *)uhash_get(fLocationNamesMap, tzIDKey);

    if (locname != NULL) {
        // gEmpty indicate the name is not available
        if (locname == gEmpty) {
            return NULL;
        }
        return locname;
    }

    // Construct location name
    UnicodeString name;
    UBool isSingleCountry = FALSE;
    UnicodeString usCountryCode;
    ZoneMeta::getSingleCountry(tzCanonicalID, usCountryCode);
    if (!usCountryCode.isEmpty()) {
        isSingleCountry = TRUE;
    } else {
        ZoneMeta::getCanonicalCountry(tzCanonicalID, usCountryCode);
    }

    if (!usCountryCode.isEmpty()) {
        char countryCode[ULOC_COUNTRY_CAPACITY];
        U_ASSERT(usCountryCode.length() < ULOC_COUNTRY_CAPACITY);
        int32_t ccLen = usCountryCode.extract(0, usCountryCode.length(), countryCode, sizeof(countryCode), US_INV);
        countryCode[ccLen] = 0;

        UnicodeString country;
        fLocaleDisplayNames->regionDisplayName(countryCode, country);

        // Format
        FieldPosition fpos;
        if (isSingleCountry) {
            // If the zone is only one zone in the country, do not add city
            Formattable param[] = {
                Formattable(country)
            };
            fRegionFormat->format(param, 1, name, fpos, status);
        } else {
            // getExemplarLocationName should retur non-empty string
            // if the time zone is associated with a region
            UnicodeString city;
            fTimeZoneNames->getExemplarLocationName(tzCanonicalID, city);

            Formattable params[] = {
                Formattable(city),
                Formattable(country)
            };
            fFallbackRegionFormat->format(params, 2, name, fpos, status);
        }
        if (U_FAILURE(status)) {
            return NULL;
        }
    }

    locname = name.isEmpty() ? NULL : fStringPool.get(name, status);
    if (U_SUCCESS(status)) {
        // Cache the result
        const UChar* cacheID = ZoneMeta::findTimeZoneID(tzCanonicalID);
        U_ASSERT(cacheID != NULL);
        if (locname == NULL) {
            // gEmpty to indicate - no location name available
            uhash_put(fLocationNamesMap, (void *)cacheID, (void *)gEmpty, &status);
        } else {
            uhash_put(fLocationNamesMap, (void *)cacheID, (void *)locname, &status);
            if (U_FAILURE(status)) {
                locname = NULL;
            } else {
                // put the name info into the trie
                GNameInfo *nameinfo = (ZNameInfo *)uprv_malloc(sizeof(GNameInfo));
                if (nameinfo != NULL) {
                    nameinfo->type = UTZGNM_LOCATION;
                    nameinfo->tzID = cacheID;
                    fGNamesTrie.put(locname, nameinfo, status);
                }
            }
        }
    }

    return locname;
}

UnicodeString&
TimeZoneGenericNames::formatGenericNonLocationName(const TimeZone& tz, UTimeZoneGenericNameType type, UDate date, UnicodeString& name) const {
    U_ASSERT(type == UTZGNM_LONG || type == UTZGNM_SHORT);
    name.setToBogus();

    const UChar* uID = ZoneMeta::getCanonicalCLDRID(tz);
    if (uID == NULL) {
        return name;
    }

    UnicodeString tzID(uID);

    // Try to get a name from time zone first
    UTimeZoneNameType nameType = (type == UTZGNM_LONG) ? UTZNM_LONG_GENERIC : UTZNM_SHORT_GENERIC;
    fTimeZoneNames->getTimeZoneDisplayName(tzID, nameType, name);

    if (!name.isEmpty()) {
        return name;
    }

    // Try meta zone
    UnicodeString mzID;
    fTimeZoneNames->getMetaZoneID(tzID, date, mzID);
    if (!mzID.isEmpty()) {
        UErrorCode status = U_ZERO_ERROR;
        UBool useStandard = FALSE;
        int32_t raw, sav;

        tz.getOffset(date, FALSE, raw, sav, status);
        if (U_FAILURE(status)) {
            return name;
        }

        if (sav == 0) {
            useStandard = TRUE;

            TimeZone *tmptz = tz.clone();
            // Check if the zone actually uses daylight saving time around the time
            BasicTimeZone *btz = NULL;
            if (dynamic_cast<OlsonTimeZone *>(tmptz) != NULL
                || dynamic_cast<SimpleTimeZone *>(tmptz) != NULL
                || dynamic_cast<RuleBasedTimeZone *>(tmptz) != NULL
                || dynamic_cast<VTimeZone *>(tmptz) != NULL) {
                btz = (BasicTimeZone*)tmptz;
            }

            if (btz != NULL) {
                TimeZoneTransition before;
                UBool beforTrs = btz->getPreviousTransition(date, TRUE, before);
                if (beforTrs
                        && (date - before.getTime() < kDstCheckRange)
                        && before.getFrom()->getDSTSavings() != 0) {
                    useStandard = FALSE;
                } else {
                    TimeZoneTransition after;
                    UBool afterTrs = btz->getNextTransition(date, FALSE, after);
                    if (afterTrs
                            && (after.getTime() - date < kDstCheckRange)
                            && after.getTo()->getDSTSavings() != 0) {
                        useStandard = FALSE;
                    }
                }
            } else {
                // If not BasicTimeZone... only if the instance is not an ICU's implementation.
                // We may get a wrong answer in edge case, but it should practically work OK.
                tmptz->getOffset(date - kDstCheckRange, FALSE, raw, sav, status);
                if (sav != 0) {
                    useStandard = FALSE;
                } else {
                    tmptz->getOffset(date + kDstCheckRange, FALSE, raw, sav, status);
                    if (sav != 0){
                        useStandard = FALSE;
                    }
                }
                if (U_FAILURE(status)) {
                    delete tmptz;
                    return name;
                }
            }
            delete tmptz;
        }
        if (useStandard) {
            UTimeZoneNameType stdNameType = (nameType == UTZNM_LONG_GENERIC)
                ? UTZNM_LONG_STANDARD : UTZNM_SHORT_STANDARD_COMMONLY_USED;
            UnicodeString stdName;
            fTimeZoneNames->getDisplayName(tzID, stdNameType, date, stdName);
            if (!stdName.isEmpty()) {
                name.setTo(stdName);

                // TODO: revisit this issue later
                // In CLDR, a same display name is used for both generic and standard
                // for some meta zones in some locales.  This looks like a data bugs.
                // For now, we check if the standard name is different from its generic
                // name below.
                UnicodeString mzGenericName;
                fTimeZoneNames->getMetaZoneDisplayName(mzID, nameType, mzGenericName);
                if (stdName.caseCompare(mzGenericName, 0) == 0) {
                    name.setToBogus();
                }
            }
        }
        if (name.isEmpty()) {
            // Get a name from meta zone
            UnicodeString mzName;
            fTimeZoneNames->getMetaZoneDisplayName(mzID, nameType, mzName);
            if (!mzName.isEmpty()) {
                // Check if we need to use a partial location format.
                // This check is done by comparing offset with the meta zone's
                // golden zone at the given date.
                UnicodeString goldenID;
                fTimeZoneNames->getReferenceZoneID(mzID, fTargetRegion, goldenID);
                if (!goldenID.isEmpty() && goldenID != tzID) {
                    TimeZone *goldenZone = TimeZone::createTimeZone(goldenID);
                    int32_t raw1, sav1;

                    // Check offset in the golden zone with wall time.
                    // With getOffset(date, false, offsets1),
                    // you may get incorrect results because of time overlap at DST->STD
                    // transition.
                    goldenZone->getOffset(date + raw + sav, TRUE, raw1, sav1, status);
                    delete goldenZone;
                    if (U_SUCCESS(status)) {
                        if (raw != raw1 || sav != sav1) {
                            // Now we need to use a partial location format
                            getPartialLocationName(tzID, mzID, (nameType == UTZNM_LONG_GENERIC), mzName, name);
                        } else {
                            name.setTo(mzName);
                        }
                    }
                } else {
                    name.setTo(mzName);
                }
            }
        }
    }
    return name;
}

UnicodeString&
TimeZoneGenericNames::getPartialLocationName(const UnicodeString& tzCanonicalID,
                        const UnicodeString& mzID, UBool isLong, const UnicodeString& mzDisplayName,
                        UnicodeString& name) const {
    name.setToBogus();
    if (tzCanonicalID.isEmpty() || mzID.isEmpty() || mzDisplayName.isEmpty()) {
        return name;
    }

    const UChar *uplname = NULL;
    TimeZoneGenericNames *nonConstThis = const_cast<TimeZoneGenericNames *>(this);
    umtx_lock(&nonConstThis->fLock);
    {
        uplname = nonConstThis->getPartialLocationName(tzCanonicalID, mzID, isLong, mzDisplayName);
    }
    umtx_unlock(&nonConstThis->fLock);

    if (uplname == NULL) {
        name.setToBogus();
    } else {
        name.setTo(TRUE, uplname, -1);
    }
    return name;
}

/*
 * This method updates the cache and must be called with a lock
 */
const UChar*
TimeZoneGenericNames::getPartialLocationName(const UnicodeString& tzCanonicalID,
                        const UnicodeString& mzID, UBool isLong, const UnicodeString& mzDisplayName) {
    U_ASSERT(!tzCanonicalID.isEmpty());
    U_ASSERT(!mzID.isEmpty());
    U_ASSERT(!mzDisplayName.isEmpty());

    PartialLocationKey key;
    key.tzID = ZoneMeta::findTimeZoneID(tzCanonicalID);
    key.mzID = ZoneMeta::findMetaZoneID(mzID);
    key.isLong = isLong;
    U_ASSERT(key.tzID != NULL && key.mzID != NULL);

    const UChar* uplname = (const UChar*)uhash_get(fPartialLocationNamesMap, (void *)&key);
    if (uplname != NULL) {
        return uplname;
    }

    UnicodeString location;
    UnicodeString usCountryCode;
    ZoneMeta::getCanonicalCountry(tzCanonicalID, usCountryCode);
    if (!usCountryCode.isEmpty()) {
        char countryCode[ULOC_COUNTRY_CAPACITY];
        U_ASSERT(usCountryCode.length() < ULOC_COUNTRY_CAPACITY);
        int32_t ccLen = usCountryCode.extract(0, usCountryCode.length(), countryCode, sizeof(countryCode), US_INV);
        countryCode[ccLen] = 0;

        UnicodeString regionalGolden;
        fTimeZoneNames->getReferenceZoneID(mzID, countryCode, regionalGolden);
        if (tzCanonicalID == regionalGolden) {
            // Use country name
            fLocaleDisplayNames->regionDisplayName(countryCode, location);
        } else {
            // Otherwise, use exemplar city name
            fTimeZoneNames->getExemplarLocationName(tzCanonicalID, location);
        }
    } else {
        fTimeZoneNames->getExemplarLocationName(tzCanonicalID, location);
        if (location.isEmpty()) {
            // This could happen when the time zone is not associated with a country,
            // and its ID is not hierarchical, for example, CST6CDT.
            // We use the canonical ID itself as the location for this case.
            location.setTo(tzCanonicalID);
        }
    }

    UErrorCode status = U_ZERO_ERROR;
    UnicodeString name;

    FieldPosition fpos;
    Formattable param[] = {
        Formattable(location),
        Formattable(mzDisplayName)
    };
    fFallbackFormat->format(param, 2, name, fpos, status);
    if (U_FAILURE(status)) {
        return NULL;
    }

    uplname = fStringPool.get(name, status);
    if (U_SUCCESS(status)) {
        // Add the name to cache
        PartialLocationKey* cacheKey = (PartialLocationKey *)uprv_malloc(sizeof(PartialLocationKey));
        if (cacheKey != NULL) {
            cacheKey->tzID = key.tzID;
            cacheKey->mzID = key.mzID;
            cacheKey->isLong = key.isLong;
            uhash_put(fPartialLocationNamesMap, (void *)cacheKey, (void *)uplname, &status);
            if (U_FAILURE(status)) {
                uprv_free(cacheKey);
            } else {
                // put the name to the local trie as well
                GNameInfo *nameinfo = (ZNameInfo *)uprv_malloc(sizeof(GNameInfo));
                if (nameinfo != NULL) {
                    nameinfo->type = isLong ? UTZGNM_LONG : UTZGNM_SHORT;
                    nameinfo->tzID = key.tzID;
                    fGNamesTrie.put(uplname, nameinfo, status);
                }
            }
        }
    }
    return uplname;
}

/*
 * This method updates the cache and must be called with a lock,
 * except initializer.
 */
void
TimeZoneGenericNames::loadStrings(const UnicodeString& tzCanonicalID) {
    // load the generic location name
    getGenericLocationName(tzCanonicalID);

    // partial location names
    UErrorCode status = U_ZERO_ERROR;

    const UnicodeString *mzID;
    UnicodeString goldenID;
    UnicodeString mzGenName;
    UTimeZoneNameType genNonLocTypes[] = {
        UTZNM_LONG_GENERIC, UTZNM_SHORT_GENERIC,
        UTZNM_UNKNOWN /*terminator*/
    };

    StringEnumeration *mzIDs = fTimeZoneNames->getAvailableMetaZoneIDs(tzCanonicalID, status);
    while ((mzID = mzIDs->snext(status))) {
        if (U_FAILURE(status)) {
            break;
        }
        // if this time zone is not the golden zone of the meta zone,
        // partial location name (such as "PT (Los Angeles)") might be
        // available.
        fTimeZoneNames->getReferenceZoneID(*mzID, fTargetRegion, goldenID);
        if (tzCanonicalID != goldenID) {
            for (int32_t i = 0; genNonLocTypes[i] != UTZNM_UNKNOWN; i++) {
                fTimeZoneNames->getMetaZoneDisplayName(*mzID, genNonLocTypes[i], mzGenName);
                if (!mzGenName.isEmpty()) {
                    // getPartialLocationName formats a name and put it into the trie
                    getPartialLocationName(tzCanonicalID, *mzID,
                        (genNonLocTypes[i] == UTZNM_LONG_GENERIC), mzGenName);
                }
            }
        }
    }
    if (mzIDs != NULL) {
        delete mzIDs;
    }
}

int32_t
TimeZoneGenericNames::findBestMatch(const UnicodeString& text, int32_t start, uint32_t types,
        UnicodeString& tzID, UTimeZoneTimeType& timeType, UErrorCode& status) const {
    timeType = UTZFMT_TIME_TYPE_UNKNOWN;
    tzID.setToBogus();

    if (U_FAILURE(status)) {
        return 0;
    }

    // Find matches in the TimeZoneNames first
    TimeZoneNameMatchInfo *tznamesMatches = findTimeZoneNames(text, start, types, status);
    if (U_FAILURE(status)) {
        return 0;
    }

    int32_t bestMatchLen = 0;
    UTimeZoneTimeType bestMatchTimeType = UTZFMT_TIME_TYPE_UNKNOWN;
    UnicodeString bestMatchTzID;
    UBool isLongStandard = FALSE;  // workaround - see the comments below

    if (tznamesMatches != NULL) {
        UnicodeString mzID;
        for (int32_t i = 0; i < tznamesMatches->size(); i++) {
            int32_t len = tznamesMatches->getMatchLength(i);
            if (len > bestMatchLen) {
                bestMatchLen = len;
                tznamesMatches->getTimeZoneID(i, bestMatchTzID);
                if (bestMatchTzID.isEmpty()) {
                    // name for a meta zone
                    tznamesMatches->getMetaZoneID(i, mzID);
                    U_ASSERT(mzID.length() > 0);
                    fTimeZoneNames->getReferenceZoneID(mzID, fTargetRegion, bestMatchTzID);
                }
                UTimeZoneNameType nameType = tznamesMatches->getNameType(i);
                switch (nameType) {
                case UTZNM_LONG_STANDARD:
                    isLongStandard = TRUE;
                case UTZNM_SHORT_STANDARD_COMMONLY_USED:
                case UTZNM_SHORT_STANDARD: // this one is never used for generic, but just in case
                    bestMatchTimeType = UTZFMT_TIME_TYPE_STANDARD;
                    break;
                case UTZNM_LONG_DAYLIGHT:
                case UTZNM_SHORT_DAYLIGHT_COMMONLY_USED:
                case UTZNM_SHORT_DAYLIGHT: // this one is never used for generic, but just in case
                    bestMatchTimeType = UTZFMT_TIME_TYPE_DAYLIGHT;
                    break;
                default:
                    bestMatchTimeType = UTZFMT_TIME_TYPE_UNKNOWN;
                }
            }
        }
        delete tznamesMatches;

        if (bestMatchLen == (text.length() - start)) {
            // Full match

            //tzID.setTo(bestMatchTzID);
            //timeType = bestMatchTimeType;
            //return bestMatchLen;

            // TODO Some time zone uses a same name for the long standard name
            // and the location name. When the match is a long standard name,
            // then we need to check if the name is same with the location name.
            // This is probably a data error or a design bug.
            if (!isLongStandard) {
                tzID.setTo(bestMatchTzID);
                timeType = bestMatchTimeType;
                return bestMatchLen;
            }
        }
    }

    // Find matches in the local trie
    TimeZoneGenericNameMatchInfo *localMatches = findLocal(text, start, types, status);
    if (U_FAILURE(status)) {
        return 0;
    }
    if (localMatches != NULL) {
        for (int32_t i = 0; i < localMatches->size(); i++) {
            int32_t len = localMatches->getMatchLength(i);

            // TODO See the above TODO. We use len >= bestMatchLen
            // because of the long standard/location name collision
            // problem. If it is also a location name, carrying
            // timeType = UTZFMT_TIME_TYPE_STANDARD will cause a
            // problem in SimpleDateFormat
            if (len >= bestMatchLen) {
                bestMatchLen = localMatches->getMatchLength(i);
                bestMatchTimeType = UTZFMT_TIME_TYPE_UNKNOWN;   // because generic
                localMatches->getTimeZoneID(i, bestMatchTzID);
            }
        }
        delete localMatches;
    }

    if (bestMatchLen > 0) {
        timeType = bestMatchTimeType;
        tzID.setTo(bestMatchTzID);
    }
    return bestMatchLen;
}

TimeZoneGenericNameMatchInfo*
TimeZoneGenericNames::findLocal(const UnicodeString& text, int32_t start, uint32_t types, UErrorCode& status) const {
    GNameSearchHandler handler(types);

    TimeZoneGenericNames *nonConstThis = const_cast<TimeZoneGenericNames *>(this);

    umtx_lock(&nonConstThis->fLock);
    {
        fGNamesTrie.search(text, start, (TextTrieMapSearchResultHandler *)&handler, status);
    }
    umtx_unlock(&nonConstThis->fLock);

    if (U_FAILURE(status)) {
        return NULL;
    }

    TimeZoneGenericNameMatchInfo *gmatchInfo = NULL;

    int32_t maxLen = 0;
    UVector *results = handler.getMatches(maxLen);
    if (results != NULL && ((maxLen == (text.length() - start)) || fGNamesTrieFullyLoaded)) {
        // perfect match
        gmatchInfo = new TimeZoneGenericNameMatchInfo(results);
        if (gmatchInfo == NULL) {
            status = U_MEMORY_ALLOCATION_ERROR;
            delete results;
            return NULL;
        }
        return gmatchInfo;
    }

    if (results != NULL) {
        delete results;
    }

    // All names are not yet loaded into the local trie.
    // Load all available names into the trie. This could be very heavy.
    umtx_lock(&nonConstThis->fLock);
    {
        if (!fGNamesTrieFullyLoaded) {
            StringEnumeration *tzIDs = TimeZone::createTimeZoneIDEnumeration(UCAL_ZONE_TYPE_CANONICAL, NULL, NULL, status);
            if (U_SUCCESS(status)) {
                const UnicodeString *tzID;
                while ((tzID = tzIDs->snext(status))) {
                    if (U_FAILURE(status)) {
                        break;
                    }
                    nonConstThis->loadStrings(*tzID);
                }
            }
            if (tzIDs != NULL) {
                delete tzIDs;
            }

            if (U_SUCCESS(status)) {
                nonConstThis->fGNamesTrieFullyLoaded = TRUE;
            }
        }
    }
    umtx_unlock(&nonConstThis->fLock);

    if (U_FAILURE(status)) {
        return NULL;
    }

    umtx_lock(&nonConstThis->fLock);
    {
        // now try it again
        fGNamesTrie.search(text, start, (TextTrieMapSearchResultHandler *)&handler, status);
    }
    umtx_unlock(&nonConstThis->fLock);

    results = handler.getMatches(maxLen);
    if (results != NULL && maxLen > 0) {
        gmatchInfo = new TimeZoneGenericNameMatchInfo(results);
        if (gmatchInfo == NULL) {
            status = U_MEMORY_ALLOCATION_ERROR;
            delete results;
            return NULL;
        }
    }

    return gmatchInfo;
}

TimeZoneNameMatchInfo*
TimeZoneGenericNames::findTimeZoneNames(const UnicodeString& text, int32_t start, uint32_t types, UErrorCode& status) const {
    TimeZoneNameMatchInfo *matchInfo = NULL;

    // Check if the target name typs is really in the TimeZoneNames
    uint32_t nameTypes = 0;
    if (types & UTZGNM_LONG) {
        nameTypes |= (UTZNM_LONG_GENERIC | UTZNM_LONG_STANDARD);
    }
    if (types & UTZGNM_SHORT) {
        nameTypes |= (UTZNM_SHORT_GENERIC | UTZNM_SHORT_STANDARD_COMMONLY_USED);
    }

    if (types) {
        // Find matches in the TimeZoneNames
        matchInfo = fTimeZoneNames->find(text, start, nameTypes, status);
    }

    return matchInfo;
}

U_NAMESPACE_END
#endif
