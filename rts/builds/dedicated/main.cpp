/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <SDL.h>

#include "Game/GameSetup.h"
#include "Game/ClientSetup.h"
#include "Game/GameData.h"
#include "Game/GameVersion.h"
#include "Net/GameServer.h"
#include "System/FileSystem/DataDirLocater.h"
#include "System/FileSystem/FileSystemInitializer.h"
#include "System/FileSystem/ArchiveScanner.h"
#include "System/FileSystem/VFSHandler.h"
#include "System/FileSystem/FileHandler.h"
#include "System/LoadSave/DemoRecorder.h"
#include "System/Log/ConsoleSink.h"
#include "System/Log/ILog.h"
#include "System/Log/DefaultFilter.h"
#include "System/LogOutput.h"
#include "System/Platform/CmdLineParams.h"
#include "System/Platform/CrashHandler.h"
#include "System/Platform/errorhandler.h"
#include "System/Config/ConfigHandler.h"
#include "System/Misc/SpringTime.h"
#include "System/GlobalConfig.h"
#include "System/Exceptions.h"
#include "System/UnsyncedRNG.h"



#define LOG_SECTION_DEDICATED_SERVER "DedicatedServer"
LOG_REGISTER_SECTION_GLOBAL(LOG_SECTION_DEDICATED_SERVER)

// use the specific section for all LOG*() calls in this source file
#ifdef LOG_SECTION_CURRENT
	#undef LOG_SECTION_CURRENT
#endif
#define LOG_SECTION_CURRENT LOG_SECTION_DEDICATED_SERVER


#ifdef __cplusplus
extern "C"
{
#endif

#ifdef __APPLE__
//FIXME: hack for SDL because of sdl-stubs
#undef main
#endif


void ParseCmdLine(int argc, char* argv[], std::string* script_txt)
{
	#undef  LOG_SECTION_CURRENT
	#define LOG_SECTION_CURRENT LOG_SECTION_DEFAULT

	std::string binaryname = argv[0];

	CmdLineParams cmdline(argc, argv);
	cmdline.SetUsageDescription("Usage: " + binaryname + " [options] path_to_script.txt");
	cmdline.AddSwitch(0,   "sync-version",       "Display program sync version (for online gaming)");
	cmdline.AddString('C', "config",             "Exclusive configuration file");
	cmdline.AddSwitch(0,   "list-config-vars",   "Dump a list of config vars and meta data to stdout");
	cmdline.AddSwitch('i', "isolation",          "Limit the data-dir (games & maps) scanner to one directory");
	cmdline.AddString(0,   "isolation-dir",      "Specify the isolation-mode data-dir (see --isolation)");
	cmdline.AddSwitch(0,   "nocolor",            "Disables colorized stdout");
	cmdline.AddSwitch('q', "quiet",              "Ignore unrecognized arguments");

	try {
		cmdline.Parse();
	} catch (const CmdLineParams::unrecognized_option& err) {
		LOG_L(L_ERROR, "%s\n", err.what());
		if (!cmdline.IsSet("quiet")) {
			cmdline.PrintUsage();
			exit(EXIT_FAILURE);
		}
	}

#ifndef WIN32
	if (!cmdline.IsSet("nocolor") && (getenv("SPRING_NOCOLOR") == NULL)) {
		// don't colorize, if our output is piped to a diff tool or file
		if (isatty(fileno(stdout)))
			log_console_colorizedOutput(true);
	}
#endif

	if (cmdline.IsSet("help")) {
		cmdline.PrintUsage();
		exit(0);
	}
	if (cmdline.IsSet("version")) {
		LOG("%s", (SpringVersion::GetFull()).c_str());
		exit(0);
	}
	if (cmdline.IsSet("sync-version")) {
		LOG("%s", (SpringVersion::GetSync()).c_str());
		exit(0);
	}


	*script_txt = cmdline.GetInputFile();
	if (script_txt->empty() && !cmdline.IsSet("list-config-vars")) {
		cmdline.PrintUsage();
		exit(1);
	}

	if (cmdline.IsSet("isolation")) {
		dataDirLocater.SetIsolationMode(true);
	}

	if (cmdline.IsSet("isolation-dir")) {
		dataDirLocater.SetIsolationMode(true);
		dataDirLocater.SetIsolationModeDir(cmdline.GetString("isolation-dir"));
	}

	const std::string configSource = cmdline.IsSet("config") ? cmdline.GetString("config") : "";

	if (cmdline.IsSet("list-config-vars")) {
		LOG_DISABLE();
		FileSystemInitializer::PreInitializeConfigHandler(configSource);
		FileSystemInitializer::InitializeLogOutput();
		LOG_ENABLE();
		ConfigVariable::OutputMetaDataMap();
		exit(0);
	}

	LOG("Run: %s", cmdline.GetCmdLine().c_str());
	FileSystemInitializer::PreInitializeConfigHandler(configSource);

	#undef  LOG_SECTION_CURRENT
	#define LOG_SECTION_CURRENT LOG_SECTION_DEDICATED_SERVER
}



int main(int argc, char* argv[])
{
	try {
		CLogOutput::LogSystemInfo();

		std::string scriptName;
		std::string scriptText;

		ParseCmdLine(argc, argv, &scriptName);

		GlobalConfig::Instantiate();
		FileSystemInitializer::InitializeLogOutput();
		FileSystemInitializer::Initialize();

		// Initialize crash reporting
		CrashHandler::Install();

		SDL_Init(SDL_INIT_TIMER);
		LOG("report any errors to Mantis or the forums.");
		LOG("loading script from file: %s", scriptName.c_str());

		CGameServer* server = NULL;
		CGameSetup* gameSetup = NULL;
		ClientSetup settings;
		CFileHandler fh(scriptName);

		if (!fh.FileExists())
			throw content_error("script does not exist in given location: " + scriptName);

		if (!fh.LoadStringData(scriptText))
			throw content_error("script cannot be read: " + scriptName);

		settings.Init(scriptText);
		gameSetup = new CGameSetup(); // to store the gamedata inside

		if (!gameSetup->Init(scriptText)) {
			// read the script provided by cmdline
			LOG_L(L_ERROR, "failed to load script %s", scriptName.c_str());
			return 1;
		}

		// Create the server, it will run in a separate thread
		GameData data;
		UnsyncedRNG rng;

		rng.Seed(gameSetup->gameSetupText.length());
		rng.Seed(scriptName.length());
		data.SetRandomSeed(rng.RandInt());

		//  Use script provided hashes if they exist
		if (gameSetup->mapHash != 0) {
			data.SetMapChecksum(gameSetup->mapHash);
			gameSetup->LoadStartPositions(false); // reduced mode
		} else {
			data.SetMapChecksum(archiveScanner->GetArchiveCompleteChecksum(gameSetup->mapName));

			CFileHandler f("maps/" + gameSetup->mapName);
			if (!f.FileExists()) {
				vfsHandler->AddArchiveWithDeps(gameSetup->mapName, false);
			}
			gameSetup->LoadStartPositions(); // full mode
		}

		if (gameSetup->modHash != 0) {
			data.SetModChecksum(gameSetup->modHash);
		} else {
			const std::string& modArchive = archiveScanner->ArchiveFromName(gameSetup->modName);
			const unsigned int modCheckSum = archiveScanner->GetArchiveCompleteChecksum(modArchive);
			data.SetModChecksum(modCheckSum);
		}

		LOG("starting server...");

		data.SetSetup(gameSetup->gameSetupText);
		server = new CGameServer(settings.hostIP, settings.hostPort, &data, gameSetup);

		while (!server->HasGameID()) {
			// wait until gameID has been generated or
			// a timeout occurs (if no clients connect)
			if (server->HasFinished()) {
				break;
			}

			spring_secs(1).sleep();
		}

		while (!server->HasFinished()) {
			static bool printData = true;

			if (printData) {
				printData = false;

				const boost::scoped_ptr<CDemoRecorder>& demoRec = server->GetDemoRecorder();
				const boost::uint8_t* gameID = (demoRec->GetFileHeader()).gameID;

				LOG("recording demo: %s", (demoRec->GetName()).c_str());
				LOG("using mod: %s", (gameSetup->modName).c_str());
				LOG("using map: %s", (gameSetup->mapName).c_str());
				LOG("GameID: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", gameID[0], gameID[1], gameID[2], gameID[3], gameID[4], gameID[5], gameID[6], gameID[7], gameID[8], gameID[9], gameID[10], gameID[11], gameID[12], gameID[13], gameID[14], gameID[15]);
			}

			// wait 1 second between checks
			spring_secs(1).sleep();
		}

		LOG("exiting");

		delete server;

		FileSystemInitializer::Cleanup();
		GlobalConfig::Deallocate();

		LOG("exited");
	}
	CATCH_SPRING_ERRORS

	return GetExitCode();
}

#if defined(WIN32) && !defined(_MSC_VER)
int WINAPI WinMain(HINSTANCE hInstanceIn, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	return main(__argc, __argv);
}
#endif

#ifdef __cplusplus
} // extern "C"
#endif
