#include "global.h"
#include "ThemeManager.h"
#include "RageFileManager.h"
#include "RageLog.h"
#include "RageException.h"
#include "RageTimer.h"
#include "RageUtil.h"
#include "IniFile.h"
#include "RageTimer.h"
#include "FontCharAliases.h"
#include "arch/ArchHooks/ArchHooks.h"
#include "arch/Dialog/Dialog.h"
#include "RageFile.h"
#if !defined(SMPACKAGE)
#include "ScreenManager.h"
#include "ActorUtil.h"
#endif
#include "Foreach.h"
#include "ThemeMetric.h"
#include "SubscriptionManager.h"
#include "LuaManager.h"
#include "ScreenDimensions.h"
#include "LocalizedString.h"
#include "SpecialFiles.h"
#include "EnumHelper.h"
#include "PrefsManager.h"

ThemeManager*	THEME = NULL;	// global object accessable from anywhere in the program

static const RString THEME_INFO_INI = "ThemeInfo.ini";

static const char *ElementCategoryNames[] = {
	"BGAnimations",
	"Fonts",
	"Graphics",
	"Sounds",
	"Other"
};
XToString( ElementCategory, NUM_ElementCategory );
StringToX( ElementCategory );


struct Theme
{
	RString sThemeName;
};
// When looking for a metric or an element, search these from head to tail.
static deque<Theme> g_vThemes;
class LoadedThemeData
{
public:
	IniFile iniMetrics;
	IniFile iniStrings;
	void ClearAll()
	{
		iniMetrics.Clear();
		iniStrings.Clear();
	}
};
LoadedThemeData *g_pLoadedThemeData = NULL;


//
// For self-registering metrics
//
#include "SubscriptionManager.h"
static SubscriptionManager<IThemeMetric> g_Subscribers;

class LocalizedStringImplThemeMetric : public ILocalizedStringImpl, public ThemeMetric<RString>
{
public:
	static ILocalizedStringImpl *Create() { return new LocalizedStringImplThemeMetric; }

	void Load( const RString& sGroup, const RString& sName )
	{
		ThemeMetric<RString>::Load( sGroup, sName );
	}

	virtual void Read()
	{
		if( m_sName != ""  &&  THEME  &&   THEME->IsThemeLoaded() )
		{
			THEME->GetString( m_sGroup, m_sName, m_currentValue );
			m_bIsLoaded = true;
		}
	}

	const RString &GetLocalized() const
	{
		if( IsLoaded() )
			return GetValue();
		else
			return m_sName;
	}
};

void ThemeManager::Subscribe( IThemeMetric *p )
{
	g_Subscribers.Subscribe( p );

	// It's ThemeManager's responsibility to make sure all of it's subscribers
	// are updated with current data.  If a metric is created after 
	// a theme is loaded, ThemeManager should update it right away (not just
	// when the theme changes).
	if( THEME && THEME->GetCurThemeName().size() )
		p->Read();
}

void ThemeManager::Unsubscribe( IThemeMetric *p )
{
	g_Subscribers.Unsubscribe( p );
}


/* We spend a lot of time doing redundant theme path lookups.  Cache results. */
static map<RString, RString> g_ThemePathCache[NUM_ElementCategory];
void ThemeManager::ClearThemePathCache()
{
	for( int i = 0; i < NUM_ElementCategory; ++i )
		g_ThemePathCache[i].clear();
}

static void FileNameToClassAndElement( const RString &sFileName, RString &sClassNameOut, RString &sElementOut )
{
	// split into class name and file name
	unsigned iIndexOfFirstSpace = sFileName.find(" ");
	if( iIndexOfFirstSpace == string::npos )	// no space
	{
		sClassNameOut = "";
		sElementOut = sFileName;
	}
	else
	{
		sClassNameOut = sFileName.Left( iIndexOfFirstSpace );
		sElementOut = sFileName.Right( sFileName.size() - iIndexOfFirstSpace - 1 );
	}
}


static RString ClassAndElementToFileName( const RString &sClassName, const RString &sElement )
{
	if( sClassName.empty() )
		return sElement;
	else
		return sClassName + " " + sElement;
}

ThemeManager::ThemeManager()
{
	THEME = this;	// so that we can Register THEME on construction

	// Register with Lua.
	{
		Lua *L = LUA->Get();
		lua_pushstring(L, "THEME");
		this->PushSelf( L );
		lua_settable( L, LUA_GLOBALSINDEX );
		LUA->Release( L );
	}

	/* We don't have any theme loaded until SwitchThemeAndLanguage is called. */
	m_sCurThemeName = "";
	m_bPseudoLocalize = false;
	
	vector<RString> arrayThemeNames;
	GetThemeNames( arrayThemeNames );
}

ThemeManager::~ThemeManager()
{
	g_vThemes.clear();
	SAFE_DELETE( g_pLoadedThemeData );

	// Unregister with Lua.
	LUA->UnsetGlobal( "THEME" );
}

void ThemeManager::GetThemeNames( vector<RString>& AddTo )
{
	GetDirListing( SpecialFiles::THEMES_DIR + "*", AddTo, true );
	StripCvs( AddTo );
}

void ThemeManager::GetSelectableThemeNames( vector<RString>& AddTo )
{
	GetThemeNames( AddTo );
	for( int i=AddTo.size()-1; i>=0; i-- )
	{
		if( !IsThemeSelectable(AddTo[i]) )
			AddTo.erase( AddTo.begin()+i );
	}
}

int ThemeManager::GetNumSelectableThemes()
{
	vector<RString> vs;
	GetSelectableThemeNames( vs );
	return vs.size();
}

bool ThemeManager::DoesThemeExist( const RString &sThemeName )
{
	vector<RString> asThemeNames;	
	GetThemeNames( asThemeNames );
	for( unsigned i=0; i<asThemeNames.size(); i++ )
	{
		if( !sThemeName.CompareNoCase(asThemeNames[i]) )
			return true;
	}
	return false;
}

bool ThemeManager::IsThemeSelectable( const RString &sThemeName )
{
	if( !DoesThemeExist(sThemeName) )
		return false;

	return sThemeName.Left(1) != "_";
}

RString ThemeManager::GetThemeDisplayName( const RString &sThemeName )
{
	RString sDir = GetThemeDirFromName(sThemeName);
	IniFile ini;
	ini.ReadFile( sDir + THEME_INFO_INI );

	RString s;
	if( ini.GetValue("ThemeInfo","DisplayName",s) )
		return s;

	return sThemeName;
}

static bool EqualsNoCase( const RString &s1, const RString &s2 )
{
       return s1.EqualsNoCase(s2);
}
void ThemeManager::GetLanguages( vector<RString>& AddTo )
{
	AddTo.clear();

	for( unsigned i = 0; i < g_vThemes.size(); ++i )
		GetLanguagesForTheme( g_vThemes[i].sThemeName, AddTo );

	// remove dupes
	sort( AddTo.begin(), AddTo.end() );
	vector<RString>::iterator it = unique( AddTo.begin(), AddTo.end(), EqualsNoCase );
	AddTo.erase(it, AddTo.end());
}

bool ThemeManager::DoesLanguageExist( const RString &sLanguage )
{
	vector<RString> asLanguages;
	GetLanguages( asLanguages );

	for( unsigned i=0; i<asLanguages.size(); i++ )
		if( sLanguage.CompareNoCase(asLanguages[i])==0 )
			return true;
	return false;
}

/* Move nodes from pFrom into pTo which don't already exist in pTo.  For
 * efficiency, nodes will be moved, not copied, so pFrom will be modified.
 * On return, the contents of pFrom will be undefined and should be deleted. */
static void MergeIniUnder( XNode *pFrom, XNode *pTo )
{
	/* Iterate over each section in pFrom. */
	XNodes::iterator it = pFrom->m_childs.begin();
	while( it != pFrom->m_childs.end() )
	{
		XNodes::iterator next = it;
		++next;

		/* If this node doesn't exist in pTo, just move the whole node. */
		XNode *pChildNode = pTo->GetChild( it->first );
		if( pChildNode == NULL )
		{
			/* We're moving the XNode without copying it, so don't use RemoveChild--it'll
			 * delete it. */
			pTo->AppendChild( it->second );
			pFrom->m_childs.erase( it );
		}
		else
		{
			/* map::insert will not overwrite existing nodes. */
			XNode *pFrom = it->second;
			FOREACHM( RString, RString, pFrom->m_attrs, it )
				pChildNode->m_attrs.insert( *it );
		}

		it = next;
	}
}

void ThemeManager::LoadThemeMetrics( deque<Theme> &theme, const RString &sThemeName_, const RString &sLanguage_ )
{
	if( g_pLoadedThemeData == NULL )
		g_pLoadedThemeData = new LoadedThemeData;

	// Don't delete and recreate LoadedThemeData.  There are references iniMetrics and iniStrings 
	// on the stack, so Clear them instead.
	g_pLoadedThemeData->ClearAll();
	g_vThemes.clear();

	RString sThemeName(sThemeName_);
	RString sLanguage(sLanguage_);

	m_sCurThemeName = sThemeName;
	m_sCurLanguage = sLanguage;

	bool bLoadedBase = false;
	while(1)
	{
		ASSERT_M( theme.size() < 20, "Circular theme fallback references detected." );

		g_vThemes.push_back( Theme() );
		Theme &t = g_vThemes.back();
		t.sThemeName = sThemeName;

		IniFile iniMetrics;
		IniFile iniStrings;
		iniMetrics.ReadFile( GetMetricsIniPath(sThemeName) );
			iniStrings.ReadFile( GetLanguageIniPath(sThemeName,SpecialFiles::BASE_LANGUAGE) );
		if( sLanguage.CompareNoCase(SpecialFiles::BASE_LANGUAGE) )
			iniStrings.ReadFile( GetLanguageIniPath(sThemeName,sLanguage) );

		bool bIsBaseTheme = !sThemeName.CompareNoCase(SpecialFiles::BASE_THEME_NAME);
		iniMetrics.GetValue( "Global", "IsBaseTheme", bIsBaseTheme );
		if( bIsBaseTheme )
			bLoadedBase = true;

		/* Read the fallback theme.  If no fallback theme is specified, and we havn't
		 * already loaded it, fall back on SpecialFiles::BASE_THEME_NAME.  That way, default theme
		 * fallbacks can be disabled with "FallbackTheme=". */
		RString sFallback;
		if( !iniMetrics.GetValue("Global","FallbackTheme",sFallback) )
		{
			if( sThemeName.CompareNoCase( SpecialFiles::BASE_THEME_NAME ) && !bLoadedBase )
				sFallback = SpecialFiles::BASE_THEME_NAME;
		}

		/* We actually want to load themes bottom-to-top, loading fallback themes
		 * first, so derived themes overwrite metrics in fallback themes. But, we
		 * need to load the derived theme first, to find out the name of the fallback
		 * theme.  Avoid having to load IniFile twice, merging the fallback theme
		 * into the derived theme that we've already loaded. */
		MergeIniUnder( &iniMetrics, &g_pLoadedThemeData->iniMetrics );
		MergeIniUnder( &iniStrings, &g_pLoadedThemeData->iniStrings );

		if( sFallback.empty() )
			break;
		sThemeName = sFallback;
	}

	/* Overlay metrics from the command line. */
	RString sMetric;
	for( int i = 0; GetCommandlineArgument( "metric", &sMetric, i ); ++i )
	{
		/* sMetric must be "foo::bar=baz".  "foo" and "bar" never contain "=", so in
		 * "foo::bar=1+1=2", "baz" is always "1+1=2".  Neither foo nor bar may be
		 * empty, but baz may be. */
		Regex re( "^([^=]+)::([^=]+)=(.*)$" );
		vector<RString> sBits;
		if( !re.Compare( sMetric, sBits ) )
			RageException::Throw( "Invalid argument \"--metric=%s\".", sMetric.c_str() );

		g_pLoadedThemeData->iniMetrics.SetValue( sBits[0], sBits[1], sBits[2] );
	}

	LOG->MapLog( "theme", "Theme: %s", m_sCurThemeName.c_str() );
	LOG->MapLog( "language", "Language: %s", m_sCurLanguage.c_str() );
}

RString ThemeManager::GetDefaultLanguage()
{
	RString sLangCode = HOOKS->GetPreferredLanguage();
	return sLangCode;
}

void ThemeManager::SwitchThemeAndLanguage( const RString &sThemeName_, const RString &sLanguage_, bool bPseudoLocalize )
{
	RString sThemeName = sThemeName_;
	RString sLanguage = sLanguage_;
#if !defined(SMPACKAGE)
	if( !IsThemeSelectable(sThemeName) )
		sThemeName = PREFSMAN->m_sTheme.GetDefault();
#endif
	if( !IsThemeSelectable(sThemeName) )
		sThemeName = SpecialFiles::BASE_THEME_NAME;
	
	ASSERT( IsThemeSelectable(sThemeName) );

	/* We havn't actually loaded the theme yet, so we can't check whether sLanguage
	 * exists.  Just check for empty. */
	if( sLanguage.empty() )
		sLanguage = GetDefaultLanguage();
	LOG->Trace("ThemeManager::SwitchThemeAndLanguage: \"%s\", \"%s\"",
		sThemeName.c_str(), sLanguage.c_str() );

	bool bNothingChanging = sThemeName == m_sCurThemeName && sLanguage == m_sCurLanguage && m_bPseudoLocalize == bPseudoLocalize;
	if( bNothingChanging )
		return;

	m_bPseudoLocalize = bPseudoLocalize;

	// Load theme metrics.  If only the language is changing, this is all
	// we need to reload.
	bool bThemeChanging = (sThemeName != m_sCurThemeName);
	LoadThemeMetrics( g_vThemes, sThemeName, sLanguage );

	// Clear the theme path cache.  This caches language-specific graphic paths, so do this
	// even if only the language is changing.
	ClearThemePathCache();
	if( bThemeChanging )
	{
#if !defined(SMPACKAGE)
		// reload common sounds
		if( SCREENMAN != NULL )
			SCREENMAN->ThemeChanged();
#endif

		/* Lua globals can use metrics which are cached, and vice versa.  Update Lua
		 * globals first; it's Lua's job to explicitly update cached metrics that it
		 * uses. */
		UpdateLuaGlobals();
	}

	// Use theme metrics for localization.
	LocalizedString::RegisterLocalizer( LocalizedStringImplThemeMetric::Create );

	ReloadSubscribers();
}

void ThemeManager::ReloadSubscribers()
{
	// reload subscribers
	if( g_Subscribers.m_pSubscribers )
	{
		FOREACHS_CONST( IThemeMetric*, *g_Subscribers.m_pSubscribers, p )
			(*p)->Read();
	}
}

void ThemeManager::ClearSubscribers()
{
	if( g_Subscribers.m_pSubscribers )
	{
		FOREACHS_CONST( IThemeMetric*, *g_Subscribers.m_pSubscribers, p )
			(*p)->Clear();
	}
}

void ThemeManager::RunLuaScripts( const RString &sMask )
{
	/* Run all script files with the given mask in Lua for all themes.  Start
	 * from the deepest fallback theme and work outwards. */
	deque<Theme>::const_iterator iter = g_vThemes.end();
	do
	{
		--iter;
		const RString &sThemeDir = GetThemeDirFromName( iter->sThemeName );
		vector<RString> asElementPaths;
		GetDirListing( sThemeDir + "Scripts/" + sMask, asElementPaths, false, true );
		for( unsigned i = 0; i < asElementPaths.size(); ++i )
		{
			const RString &sPath = asElementPaths[i];
			LOG->Trace( "Loading \"%s\" ...", sPath.c_str() );
			LuaHelpers::RunScriptFile( sPath );
		}
	}
	while( iter != g_vThemes.begin() );
}

void ThemeManager::UpdateLuaGlobals()
{
#if !defined(SMPACKAGE)
	/* explicitly refresh cached metrics that we use. */
	ScreenDimensions::ReloadScreenDimensions();

	RunLuaScripts( "*.lua" );
#endif
}

RString ThemeManager::GetThemeDirFromName( const RString &sThemeName )
{
	return SpecialFiles::THEMES_DIR + sThemeName + "/";
}

struct CompareLanguageTag
{
	RString m_sLanguageString;
	CompareLanguageTag( const RString &sLang )
	{
		m_sLanguageString = RString("(lang ") + sLang + ")";
		LOG->Trace( "try \"%s\"", sLang.c_str() );
		m_sLanguageString.MakeLower();
	}

	bool operator()( const RString &sFile ) const
	{
		RString sLower( sFile );
		sLower.MakeLower();
		size_t iPos = sLower.find( m_sLanguageString );
		return iPos != RString::npos;
	}
};

/*
 * If there's more than one result, check for language tags.  For example,
 *
 * ScreenCompany graphic (lang English).png
 * ScreenCompany graphic (lang French).png
 *
 * We still want to warn for ambiguous results.  Use std::unique to filter
 * files with the current language tag to the top, so choosing "ignore" from
 * the multiple-match dialog will cause it to default to the first entry, so
 * it'll still use a preferred language match if there were any.
 */
void ThemeManager::FilterFileLanguages( vector<RString> &asPaths )
{
	if( asPaths.size() <= 1 )
		return;
	vector<RString>::iterator it =
		partition( asPaths.begin(), asPaths.end(), CompareLanguageTag(m_sCurLanguage) );

	int iDist = distance( asPaths.begin(), it );
	if( iDist == 0 )
	{
		/* We didn't find any for the current language.  Try BASE_LANGUAGE. */
		it = partition( asPaths.begin(), asPaths.end(), CompareLanguageTag(SpecialFiles::BASE_LANGUAGE) );
		iDist = distance( asPaths.begin(), it );
	}

	if( iDist == 1 )
		asPaths.erase( it, asPaths.end() );
}

RString ThemeManager::GetPathToRaw( const RString &sThemeName_, ElementCategory category, const RString &sClassName_, const RString &sElement_ ) 
{
	/* Ugly: the parameters to this function may be a reference into g_vThemes, or something
	 * else that might suddenly go away when we call ReloadMetrics. */
	const RString sThemeName = sThemeName_;
	const RString sClassName = sClassName_;
	const RString sElement = sElement_;

try_element_again:

	const RString sThemeDir = GetThemeDirFromName( sThemeName );
	const RString &sCategory = ElementCategoryToString(category);

	vector<RString> asElementPaths;

	// If sFileName already has an extension, we're looking for a specific file
	bool bLookingForSpecificFile = sElement.find_last_of('.') != sElement.npos;

	if( bLookingForSpecificFile )
	{
		GetDirListing( sThemeDir + sCategory+"/"+ClassAndElementToFileName(sClassName,sElement), asElementPaths, false, true );
	}
	else	// look for all files starting with sFileName that have types we can use
	{
		vector<RString> asPaths;
		GetDirListing( sThemeDir + sCategory + "/" + ClassAndElementToFileName(sClassName,sElement) + "*",
						asPaths, false, true );

		for( unsigned p = 0; p < asPaths.size(); ++p )
		{
			static const char *masks[NUM_ElementCategory][14] = {
				{ "redir", "xml", "png", "jpg", "bmp", "gif","avi", "mpg", "mpeg", "txt", "", NULL},
				{ "redir", "ini", NULL },
				{ "redir", "xml", "png", "jpg", "bmp", "gif","avi", "mpg", "mpeg", "txt", "", NULL},
				{ "redir", "mp3", "ogg", "wav", NULL },
				{ "*", NULL },
			};		
			const char **asset_masks = masks[category];

			const RString ext = GetExtension( asPaths[p] );

			for( int i = 0; asset_masks[i]; ++i )
			{
				/* No extension means directories. */
				if( asset_masks[i][0] == 0 )
				{
					if( !IsADirectory(asPaths[p]) )
						continue;
					
#ifdef DEBUG
					// Ignore empty directories so that we don't have to wait to test changes until 
					// CVS is updated and prunes the empties.
					vector<RString> vs;
					GetDirListing( asPaths[p]+"/*", vs, false, false );
					StripCvs( vs );
					bool bDirIsEmpty = vs.empty();
					if( bDirIsEmpty )
						continue;
#endif
				}

				if( ext == asset_masks[i] || !strcmp(asset_masks[i], "*") )
				{
					asElementPaths.push_back( asPaths[p] );
					break;
				}
			}
		}
	}
	

	if( asElementPaths.size() == 0 )
		return RString();	// This isn't fatal.

	FilterFileLanguages( asElementPaths );

	if( asElementPaths.size() > 1 )
	{
		/* Why do we flush here? Aborting ends, retrying calls ReloadMetrics which will flush
		 * and ignoring doesn't do anything further, nothing that would seem to require
		 * flushing the cache. -- Steve */
		FILEMAN->FlushDirCache( GetThemeDirFromName(sThemeName) );
		g_ThemePathCache[category].clear();

		RString message = ssprintf( 
			"ThemeManager:  There is more than one theme element that matches "
			"'%s/%s/%s'.  Please remove all but one of these matches.",
			sThemeName.c_str(), sCategory.c_str(), ClassAndElementToFileName(sClassName,sElement).c_str() );

		switch( Dialog::AbortRetryIgnore(message) )
		{
		case Dialog::abort:
			RageException::Throw( "%s", message.c_str() ); 
			break;
		case Dialog::retry:
			ReloadMetrics();
			goto try_element_again;
		case Dialog::ignore:
			break;
		}
	}


	RString sPath = asElementPaths[0];
	bool bIsARedirect = GetExtension(sPath).CompareNoCase("redir")==0;

	if( !bIsARedirect )
		return sPath;

	RString sNewFileName;
	GetFileContents( sPath, sNewFileName, true );

	RString sNewClassName, sNewFile;
	FileNameToClassAndElement(sNewFileName, sNewClassName, sNewFile);
	
	/* Search again.  For example, themes/default/Fonts/foo might redir
	 * to "Hello"; but "Hello" might be overridden in themes/hot pink/Fonts/Hello. */
	/* Important: We need to do a full search.  For example, BG redirs in
	 * the default theme point to "_shared background", and themes override
	 * just "_shared background"; the redirs in the default theme should end
	 * up resolving to the overridden background. */
	/* Use GetPathToOptional because we don't want report that there's an element
	 * missing.  Instead we want to report that the redirect is invalid. */
	RString sNewPath = GetPath(category, sNewClassName, sNewFile, true);

	if( !sNewPath.empty() )
		return sNewPath;

	RString sMessage = ssprintf(
			"ThemeManager:  The redirect '%s' points to the file '%s', which does not exist. "
			"Verify that this redirect is correct.",
			sPath.c_str(), sNewFileName.c_str());

	switch( Dialog::AbortRetryIgnore(sMessage) )
	{
	case Dialog::retry:
		ReloadMetrics();
		goto try_element_again;
	case Dialog::ignore:
		return GetPath( category, "", "_missing" );
	}

	RageException::Throw( "%s", sMessage.c_str() ); 
}

RString ThemeManager::GetPathToAndFallback( ElementCategory category, const RString &sClassName_, const RString &sElement ) 
{
	RString sClassName( sClassName_ );

	int n = 100;
	while( n-- )
	{
		FOREACHD_CONST( Theme, g_vThemes, iter )
		{
			// search with requested name
			RString sRet = GetPathToRaw( iter->sThemeName, category, sClassName, sElement );
			if( !sRet.empty() )
				return sRet;
		}

		if( sClassName.empty() )
			return RString();

		// search fallback name (if any)
		sClassName = GetClassFallback( sClassName );
		if( sClassName.empty() )
			return RString();
	}

	RageException::Throw( "Infinite recursion looking up theme element \"%s\"",
		ClassAndElementToFileName(sClassName, sElement).c_str() );
}

RString ThemeManager::GetPath( ElementCategory category, const RString &sClassName_, const RString &sElement_, bool bOptional ) 
{
	/* Ugly: the parameters to this function may be a reference into g_vThemes, or something
	 * else that might suddenly go away when we call ReloadMetrics. */
	const RString sClassName = sClassName_;
	const RString sElement = sElement_;

	RString sFileName = ClassAndElementToFileName( sClassName, sElement );

	map<RString, RString> &Cache = g_ThemePathCache[category];
	{
		map<RString, RString>::const_iterator i;
		
		i = Cache.find( sFileName );
		if( i != Cache.end() )
			return i->second;
	}
	
try_element_again:
	
	// search the current theme
	RString ret = GetPathToAndFallback( category, sClassName, sElement );
	if( !ret.empty() )	// we found something
	{
		Cache[sFileName] = ret;
		return ret;
	}

	if( bOptional )
	{
		Cache[sFileName] = "";
		return RString();
	}

	const RString &sCategory = ElementCategoryToString(category);

	/* We can't fall back on _missing in Other: the file types are unknown. */
	RString sMessage = "The theme element \"" + sCategory + "/" + sFileName +"\" is missing.";
	Dialog::Result res;
	if( category != EC_OTHER )
		res = Dialog::AbortRetryIgnore( sMessage, "MissingThemeElement" );
	else
		res = Dialog::AbortRetry( sMessage, "MissingThemeElement" );
	switch( res )
	{
	case Dialog::retry:
		ReloadMetrics();
		goto try_element_again;
	case Dialog::ignore:
		LOG->UserLog( "Theme element", sCategory + '/' + sFileName,
			      "could not be found in \"%s\" or \"%s\".",
			      GetThemeDirFromName(m_sCurThemeName).c_str(), 
			      GetThemeDirFromName(SpecialFiles::BASE_THEME_NAME).c_str() );

		/* Err? */
		if( sFileName == "_missing" )
			RageException::Throw( "\"_missing\" isn't present in \"%s%s\".", GetThemeDirFromName(SpecialFiles::BASE_THEME_NAME).c_str(), sCategory.c_str() );

		Cache[sFileName] = GetPath( category, "", "_missing" );
		return Cache[sFileName];
	case Dialog::abort:
		LOG->UserLog( "Theme element", sCategory + '/' + sFileName,
			      "could not be found in \"%s\" or \"%s\".",
			      GetThemeDirFromName(m_sCurThemeName).c_str(), 
			      GetThemeDirFromName(SpecialFiles::BASE_THEME_NAME).c_str() );
		RageException::Throw( "Theme element \"%s/%s\" could not be found in \"%s\" or \"%s\".", 
			sCategory.c_str(),
			sFileName.c_str(), 
			GetThemeDirFromName(m_sCurThemeName).c_str(), 
			GetThemeDirFromName(SpecialFiles::BASE_THEME_NAME).c_str() );
	DEFAULT_FAIL( res );
	}
}


RString ThemeManager::GetMetricsIniPath( const RString &sThemeName )
{
	return GetThemeDirFromName( sThemeName ) + SpecialFiles::METRICS_FILE;
}

bool ThemeManager::HasMetric( const RString &sClassName, const RString &sValueName )
{
	RString sThrowAway;
	return GetMetricRawRecursive( g_pLoadedThemeData->iniMetrics, sClassName, sValueName, sThrowAway );
}

bool ThemeManager::HasString( const RString &sClassName, const RString &sValueName )
{
	RString sThrowAway;
	return GetMetricRawRecursive( g_pLoadedThemeData->iniStrings, sClassName, sValueName, sThrowAway );
}

static LocalizedString RELOADED_METRICS( "ThemeManager", "Reloaded metrics" );
void ThemeManager::ReloadMetrics()
{
	FILEMAN->FlushDirCache( GetCurThemeDir() );

	// force a reload of the metrics cache
	LoadThemeMetrics( g_vThemes, m_sCurThemeName, m_sCurLanguage );

#if !defined(SMPACKAGE)
	if( SCREENMAN )
		SCREENMAN->SystemMessage( RELOADED_METRICS );
#endif

	ClearThemePathCache();
}


RString ThemeManager::GetClassFallback( const RString &sClassName )
{
	// always look in iniMetrics for "Fallback"
	RString sFallback;
	if( !GetMetricRawRecursive(g_pLoadedThemeData->iniMetrics,sClassName,"Fallback",sFallback) )
		return RString();

	RString sRet;
	LuaHelpers::RunExpressionS( sFallback, sRet );
	return sRet;
}

bool ThemeManager::GetMetricRawRecursive( const IniFile &ini, const RString &sClassName_, const RString &sValueName, RString &sOut )
{
	ASSERT( sValueName != "" );
	RString sClassName( sClassName_ );

	ASSERT( g_pLoadedThemeData );

	int n = 100;
	while( n-- )
	{
		if( ini.GetValue(sClassName,sValueName,sOut) )
			return true;

		if( !sValueName.compare("Fallback") )
			return false;

		sClassName = GetClassFallback( sClassName );
		if( sClassName.empty() )
			return false;
	}

	RageException::Throw( "Infinite recursion looking up theme metric \"%s::%s\".", sClassName.c_str(), sValueName.c_str() );
}

RString ThemeManager::GetMetricRaw( const IniFile &ini, const RString &sClassName_, const RString &sValueName_ )
{
	/* Ugly: the parameters to this function may be a reference into g_vThemes, or something
	 * else that might suddenly go away when we call ReloadMetrics. */
	const RString sClassName = sClassName_;
	const RString sValueName = sValueName_;

	while( true )
	{
		RString ret;
		if( ThemeManager::GetMetricRawRecursive(ini, sClassName, sValueName, ret) )
			return ret;
		
		RString sCurMetricPath = GetMetricsIniPath( m_sCurThemeName );
		RString sDefaultMetricPath = GetMetricsIniPath( SpecialFiles::BASE_THEME_NAME );
		RString sMessage = ssprintf( "The theme metric \"%s::%s\" is missing.  Correct this and click Retry, or Cancel to break.",
					     sClassName.c_str(), sValueName.c_str() );
		switch( Dialog::AbortRetryIgnore(sMessage) )
		{
			case Dialog::abort:
				RageException::Throw( "Theme metric \"%s::%s\" could not be found in \"%s\"' or \"%s\".", 
						      sClassName.c_str(), sValueName.c_str(), sCurMetricPath.c_str(), 
						      sDefaultMetricPath.c_str() );
			case Dialog::retry:
				ReloadMetrics();
				continue;
			case Dialog::ignore:
				LOG->UserLog( "Theme metric", sClassName + " : " + sValueName,
					      "could not be found in \"%s\" or \"%s\".",
					      sCurMetricPath.c_str(), sDefaultMetricPath.c_str() );
				return RString();
			default:
				ASSERT(0);
		}
	}
}

template<typename T>
void GetAndConvertMetric( const RString &sClassName, const RString &sValueName, T &out )
{
	Lua *L = LUA->Get();

	THEME->PushMetric( L, sClassName, sValueName );
	LuaHelpers::FromStack( L, out, -1 );
	lua_pop( L, 1 );

	LUA->Release(L);
}

/* Get a string metric. */
RString ThemeManager::GetMetric( const RString &sClassName, const RString &sValueName )
{
	RString sRet;
	GetAndConvertMetric( sClassName, sValueName, sRet );
	return sRet;
}

int ThemeManager::GetMetricI( const RString &sClassName, const RString &sValueName )
{
	int iRet = 0;
	GetAndConvertMetric( sClassName, sValueName, iRet );
	return iRet;
}

float ThemeManager::GetMetricF( const RString &sClassName, const RString &sValueName )
{
	float fRet = 0;
	GetAndConvertMetric( sClassName, sValueName, fRet );
	return fRet;
}

bool ThemeManager::GetMetricB( const RString &sClassName, const RString &sValueName )
{
	bool bRet = 0;
	GetAndConvertMetric( sClassName, sValueName, bRet );
	return bRet;
}

RageColor ThemeManager::GetMetricC( const RString &sClassName, const RString &sValueName )
{
	RageColor ret;
	GetAndConvertMetric( sClassName, sValueName, ret );
	return ret;
}

LuaReference ThemeManager::GetMetricR( const RString &sClassName, const RString &sValueName )
{
	LuaReference ref;
	GetMetric( sClassName, sValueName, ref );
	return ref;
}

void ThemeManager::PushMetric( Lua *L, const RString &sClassName, const RString &sValueName )
{
	RString sValue = GetMetricRaw( g_pLoadedThemeData->iniMetrics, sClassName, sValueName );

	RString sName = ssprintf( "%s::%s", sClassName.c_str(), sValueName.c_str() );
	if( EndsWith(sValueName, "Command") )
	{
		ActorUtil::ParseActorCommands( L, sValue, sName );
	}
	else
	{
		LuaHelpers::PrepareExpression( sValue );
		LuaHelpers::RunExpression( L, sValue, sName );
	}
}

void ThemeManager::GetMetric( const RString &sClassName, const RString &sValueName, LuaReference &valueOut )
{
	Lua *L = LUA->Get();
	PushMetric( L, sClassName, sValueName );
	valueOut.SetFromStack( L );
	LUA->Release( L );
}

#if !defined(SMPACKAGE)
apActorCommands ThemeManager::GetMetricA( const RString &sClassName, const RString &sValueName )
{
	LuaReference *pRef = new LuaReference;
	GetMetric( sClassName, sValueName, *pRef );
	return apActorCommands( pRef );
}
#endif

void ThemeManager::EvaluateString( RString &sText )
{
	/* If the string begins with an @, then this is a Lua expression
	 * that should be evaluated immediately.
	 * Still do font aliases on the resulting string. */
	LuaHelpers::RunAtExpressionS( sText );

	FontCharAliases::ReplaceMarkers( sText );
}

RString ThemeManager::GetNextTheme()
{
	vector<RString> as;
	GetThemeNames( as );
	unsigned i;
	for( i=0; i<as.size(); i++ )
		if( as[i].CompareNoCase(m_sCurThemeName)==0 )
			break;
	int iNewIndex = (i+1)%as.size();
	return as[iNewIndex];
}

void ThemeManager::GetLanguagesForTheme( const RString &sThemeName, vector<RString>& asLanguagesOut )
{
	RString sLanguageDir = GetThemeDirFromName(sThemeName) + SpecialFiles::LANGUAGES_SUBDIR;
	vector<RString> as;
	GetDirListing( sLanguageDir + "*.ini", as );
	
	// strip out metrics.ini
	for( int i=as.size()-1; i>=0; i-- )
	{
		if( as[i].CompareNoCase(SpecialFiles::METRICS_FILE)==0 )
			as.erase( as.begin()+i );
		// strip ".ini"
		as[i] = as[i].Left( as[i].size()-4 );
	}

	asLanguagesOut.insert( asLanguagesOut.end(), as.begin(), as.end() );
}

RString ThemeManager::GetLanguageIniPath( const RString &sThemeName, const RString &sLanguage )
{
	return GetThemeDirFromName(sThemeName) + SpecialFiles::LANGUAGES_SUBDIR + sLanguage + ".ini";
}

void ThemeManager::GetOptionNames( vector<RString>& AddTo )
{
	const XNode *cur = g_pLoadedThemeData->iniStrings.GetChild( "OptionNames" );
	if( cur )
	{
		FOREACH_CONST_Attr( cur, p )
			AddTo.push_back( p->first );
	}
}

static RString PseudoLocalize( RString s )
{
	s.Replace( "a", "àá" );
	s.Replace( "A", "ÀÀ" );
	s.Replace( "e", "éé" );
	s.Replace( "E", "ÉÉ" );
	s.Replace( "i", "íí" );
	s.Replace( "I", "ÍÍ" );
	s.Replace( "o", "óó" );
	s.Replace( "O", "ÓÓ" );
	s.Replace( "u", "üü" );
	s.Replace( "U", "ÜÜ" );
	s.Replace( "n", "ñ" );
	s.Replace( "N", "Ñ" );
	s.Replace( "c", "ç" );
	s.Replace( "C", "Ç" );
	// transformations that help expose punctuation assumptions
	//s.Replace( ":", " :" );	// this messes up "::" help text tip separator markers
	s.Replace( "?", " ?" );
	s.Replace( "!", " !" );

	return s;
}

RString ThemeManager::GetString( const RString &sClassName, const RString &sValueName_ )
{
	RString sValueName = sValueName_;

	// TODO: Are there escape rules for this?
	DEBUG_ASSERT( sValueName.find('=') == sValueName.npos );

	// TODO: Move this escaping into IniFile?
	sValueName.Replace( "\r\n", "\\n" );
	sValueName.Replace( "\n", "\\n" );

	RString s = GetMetricRaw( g_pLoadedThemeData->iniStrings, sClassName, sValueName );
	FontCharAliases::ReplaceMarkers( s );

	// Don't EvalulateString.  Strings are raw and shouldn't allow Lua.
	//EvaluateString( s );

	// Write stubs for missing strings into every language file.
	/*
	if( !ThemeManager::GetMetricRawRecursive( sClassName, sValueName, s ) )
	{
		RString sFile = GetLanguageIniPath( SpecialFiles::BASE_THEME_NAME, m_sCurLanguage );
		IniFile ini;
		ini.ReadFile( sFile );
		s = sValueName;
		ini.SetValue( sClassName, sValueName, s );
		ini.WriteFile( sFile );

		LoadThemeMetrics( g_vThemes, m_sCurThemeName, m_sCurLanguage );
	}
	*/

	s.Replace( "\\n", "\n" );

	if( m_bPseudoLocalize )
	{
		// pseudolocalize ignoring replace markers.  e.g.: "%{steps} steps: %{author}"
		RString sTranslated;

		for( ; true; )
		{
			RString::size_type pos = s.find( "%{" );
			if( pos == s.npos )
			{
				sTranslated += PseudoLocalize( s );
				s = RString();
				break;
			}
			else
			{
				sTranslated += PseudoLocalize( s.substr(0,pos) );
				s.erase( s.begin(), s.begin()+pos );
			}
			
			pos = s.find( "}" );
			sTranslated += s.substr(0,pos+1);
			s.erase( s.begin(), s.begin()+pos+1 );
		}

		s = sTranslated;
	}

	return s;
}

void ThemeManager::GetMetricsThatBeginWith( const RString &sClassName_, const RString &sValueName, set<RString> &vsValueNamesOut )
{
	RString sClassName( sClassName_ );
	while( !sClassName.empty() )
	{
		const XNode *cur = g_pLoadedThemeData->iniMetrics.GetChild( sClassName );
		if( cur != NULL )
		{
			// Iterate over all metrics that match.
			for( XAttrs::const_iterator j = cur->m_attrs.lower_bound( sValueName ); j != cur->m_attrs.end(); ++j )
			{
				const RString &sv = j->first;
				if( sv.Left(sValueName.size()) == sValueName )
					vsValueNamesOut.insert( sv );
				else	// we passed the last metric that matched sValueName
					break;
			}
		}

		// put the fallback (if any) in sClassName
		sClassName = GetClassFallback( sClassName );
	}
}


RString ThemeManager::GetBlankGraphicPath()
{
	return SpecialFiles::THEMES_DIR + SpecialFiles::BASE_THEME_NAME + "/" + ElementCategoryToString(EC_GRAPHICS) + "/_blank.png";
}

// lua start
#include "LuaBinding.h"

class LunaThemeManager: public Luna<ThemeManager>
{
public:
	static int GetMetric( T* p, lua_State *L )			{ p->PushMetric( L, SArg(1),SArg(2) ); return 1; }
	static int GetString( T* p, lua_State *L )			{ lua_pushstring(L, p->GetString(SArg(1),SArg(2)) ); return 1; }
	static int GetPathG( T* p, lua_State *L )			{ lua_pushstring(L, p->GetPathG(SArg(1),SArg(2)) ); return 1; }
	static int GetPathB( T* p, lua_State *L )			{ lua_pushstring(L, p->GetPathB(SArg(1),SArg(2)) ); return 1; }
	static int GetPathS( T* p, lua_State *L )			{ lua_pushstring(L, p->GetPathS(SArg(1),SArg(2)) ); return 1; }
	static int GetNumSelectableThemes( T* p, lua_State *L )		{ lua_pushnumber(L, p->GetNumSelectableThemes() ); return 1; }

	LunaThemeManager()
	{
		ADD_METHOD( GetMetric );
		ADD_METHOD( GetString );
		ADD_METHOD( GetPathG );
		ADD_METHOD( GetPathB );
		ADD_METHOD( GetPathS );
		ADD_METHOD( GetNumSelectableThemes );
	}
};

LUA_REGISTER_CLASS( ThemeManager )
// lua end


/*
 * (c) 2001-2004 Chris Danford
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
