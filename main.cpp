#include "pch.h"
#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
//	#include "vld.h" // mem leaks +
#endif
#include "CGame.h"
#include "../vdrift/game.h"
#include "../vdrift/logging.h"
#include "../vdrift/pathmanager.h"
#include "../vdrift/settings.h"
#include "../network/enet-wrapper.hpp"

#include <string>
#include <sstream>
#include <list>
#include <ostream>

#include <OgrePlatform.h>
#include <OgreStringConverter.h>
#include <boost/filesystem.hpp>
#include <locale.h>

//patch start joseph
#include <pthread.h>
//patch end

//  load settings from default file
void LoadDefaultSet(SETTINGS* settings, std::string setFile)
{
	settings->Load(PATHMANAGER::GameConfigDir() + "/game-default.cfg");
	settings->Save(setFile);
	//  delete old keys.xml too
	std::string sKeys = PATHMANAGER::UserConfigDir() + "/keys.xml";
	if (boost::filesystem::exists(sKeys))
		boost::filesystem::rename(sKeys, PATHMANAGER::UserConfigDir() + "/keys_old.xml");
}


//  . . . . . . . . . .  MAIN  . . . . . . . . . .
#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
	int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPTSTR lpCmdLine, int nCmdShow)
#else
	int main(int argc, char* argv[])
#endif
{
	setlocale(LC_NUMERIC, "C");

	PATHMANAGER::Init(std::cout, std::cerr);


	std::streambuf* oldCout = std::cout.rdbuf();
	std::streambuf* oldCerr = std::cerr.rdbuf();
	#if 0
    std::string po = PATHMANAGER::UserConfigDir() + "/ogre.out";
    std::ofstream out(po.c_str());
    std::cout.rdbuf(out.rdbuf());  // redirect std::cout to out.txt
    #endif

	#if 1
    std::string pa = PATHMANAGER::UserConfigDir() + "/ogre.err";
    std::ofstream oute(pa.c_str());
    std::cerr.rdbuf(oute.rdbuf());  // redirect std::cerr to oute.txt
    #endif


	// Open the log file
	std::string logfilename = PATHMANAGER::UserConfigDir() + "/log.txt";
	std::ofstream logfile(logfilename.c_str());
	if (!logfile)
	{
		std::cerr << "Couldn't open log file: " << logfilename << std::endl;
		return EXIT_FAILURE;
	}
	
	// Set up logging arrangement
	logging::splitterstreambuf infosplitter(std::cout, logfile);	std::ostream infosplitterstream(&infosplitter);
	logging::splitterstreambuf errorsplitter(std::cerr, logfile);	std::ostream errorsplitterstream(&errorsplitter);
	logging::logstreambuf infolog("INFO: ", infosplitterstream);	//logstreambuf infolog("INFO: ", logfile);
	logging::logstreambuf errorlog("ERROR: ", errorsplitterstream);

	// Primary logging ostreams
	std::ostream info_output(&infolog);
	std::ostream error_output(&errorlog);/**/

	// HACK: We initialize paths a second time now that we have the output streams
	PATHMANAGER::Init(info_output, error_output, false);  // false - same paths, dont log

	// Initialize networking
	net::ENetContainer enet;


	///  Load Settings
	//----------------------------------------------------------------
	SETTINGS* settings = new SETTINGS();
	std::string setFile = PATHMANAGER::SettingsFile();
	
	if (!PATHMANAGER::FileExists(setFile))
	{
		info_output << "Settings not found - loading defaults." << std::endl;
		LoadDefaultSet(settings,setFile);
	}
	settings->Load(setFile);  // LOAD
	if (settings->version != SET_VER)  // loaded older, use default
	{
		info_output << "Settings found, but older version - loading defaults." << std::endl;
		boost::filesystem::rename(setFile, PATHMANAGER::UserConfigDir() + "/game_old.cfg");
		LoadDefaultSet(settings,setFile);
		settings->Load(setFile);  // LOAD
	}
	
	// HACK: we initialize paths a second time now that we have the output streams
	PATHMANAGER::Init(info_output, error_output);

	
	//  helper for testing networked game on 1 computer
	//  use number > 0 in command parameter,  uses own ogre.log and adds it to nick and port
	int num = -1;
	#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
	if (lpCmdLine)
		num = Ogre::StringConverter::parseInt(lpCmdLine);
	#else
	if (argc > 1)
		num = Ogre::StringConverter::parseInt(argv[1]);
	#endif
	if (num > 0)
	{
		settings->net_local_plr = num;
		settings->local_port += num;
		settings->nickname += Ogre::StringConverter::toString(num);
	}


	///  Game start
	//----------------------------------------------------------------
	GAME* pGame = new GAME(info_output, error_output, settings);
	std::list <std::string> args;//(argv, argv + argc);
	pGame->Start(args);  //game.End();

	App* pApp = new App(settings, pGame);
	pGame->app = pApp;

	try
	{
        pthread_t t1;
        pthread_create( &t1, NULL, &GAME::custom_duty_helper,pGame);
		#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
			pApp->Run( settings->ogre_dialog || lpCmdLine[0]!=0 );  //Release change-
		#else
			pApp->Run( settings->ogre_dialog);
		#endif
	}
	catch (Ogre::Exception& e)
	{
		#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
			MessageBoxA( NULL, e.getFullDescription().c_str(), "An exception has occured!", MB_OK | MB_ICONERROR | MB_TASKMODAL);
		#else
			std::cerr << "An exception has occured: " << e.getFullDescription().c_str() << std::endl;
		#endif
	}

	info_output << "Exiting" << std::endl;
	delete pApp;
	delete pGame;
	delete settings;

	std::cout.rdbuf(oldCout);
	std::cerr.rdbuf(oldCerr);

	return 0;
}
