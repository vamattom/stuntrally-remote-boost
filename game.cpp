#include "pch.h"
#include "game.h"
#include "unittest.h"
#include "joepack.h"
#include "matrix4.h"
#include "configfile.h"
#include "cardefs.h"
#include <math.h>

#include "numprocessors.h"
#include "performance_testing.h"
#include "quickprof.h"
#include "tracksurface.h"
#include "forcefeedback.h"
#include "../ogre/common/Def_Str.h"
#include "../ogre/common/data/SceneXml.h"
#include "../ogre/common/CScene.h"
#include "../ogre/CGame.h"
#include "../ogre/CInput.h"
#include "../ogre/FollowCamera.h"
#include "../oics/ICSInputControlSystem.h"
#include <OgreTimer.h>

#define M_PI  3.14159265358979323846
using namespace std;


///  ctor
GAME::GAME(ostream & info_out, ostream & err_out, SETTINGS* pSettings) :
	settings(pSettings), info_output(info_out), error_output(err_out),
	frame(0), displayframe(0), clocktime(0), target_time(0),
	//framerate(0.01f),  ///~  0.004+  o:0.01
	fps_track(10,0), fps_position(0), fps_min(0), fps_max(0),
	multithreaded(false), benchmode(false), dumpfps(false),
	pause(false), debugmode(false), profilingmode(false),
	particle_timer(0), race_laps(0),
	track(info_out, err_out), /*tracknode(NULL),*/
	framerate(1.0 / pSettings->game_fq),
	app(NULL),
	tire_ref_id(0),
	reloadSimNeed(0),reloadSimDone(0)
{
	track.pGame = this;
	carcontrols_local.first = NULL;
	//  sim iv from settings
	collision.fixedTimestep = 1.0 / pSettings->blt_fq;
	collision.maxSubsteps = pSettings->blt_iter;
}


//  start the game with the given arguments
void GAME::Start(list <string> & args)
{
	if (!ParseArguments(args))
		return;

	info_output << "Starting VDrift-Ogre: 2010-05-01, O/S: ";
	#ifdef _WIN32
		info_output << "Windows" << endl;
	#elif defined(__APPLE__)
		info_output << "Apple" << endl;
	#else
		info_output << "Unix-like" << endl;
	#endif

	//settings->Load(PATHMANAGER::GetSettingsFile());

	carcontrols_local.second.Reset();

	InitializeSound(); //if sound initialization fails, that's okay, it'll disable itself

	//initialize GUI
	map<string, string> optionmap;
	LoadSaveOptions(LOAD, optionmap);

	//initialize force feedback
	#ifdef ENABLE_FORCE_FEEDBACK
		forcefeedback.reset(new FORCEFEEDBACK(settings->ff_device, error_output, info_output));
		ff_update_time = 0;
	#endif
	
	//ReloadSimData();  // later (need game.sim_mode)
}

void GAME::ReloadSimData()  /// New
{
	LoadTires();
	LoadAllSurfaces();
	LoadSusp();

	info_output << "Carsim: " << settings->game.sim_mode << ". Loaded: " << tires.size() << " tires, " << surfaces.size() << " surfaces, " << suspS.size() << "=" << suspD.size() << " suspensions." << endl;
}


///  Surfaces  all in data/cars/surfaces.cfg
//------------------------------------------------------------------------------------------------------------------------------
bool GAME::LoadAllSurfaces()
{
	surfaces.clear();
	surf_map.clear();

	string path, file = "/" + settings->game.sim_mode + "/surfaces.cfg";
	path = PATHMANAGER::CarSimU() + file;
	if (!PATHMANAGER::FileExists(path))  // user or orig
		path = PATHMANAGER::CarSim() + file;
	else
		info_output << "Using user surfaces" << endl;
	
	CONFIGFILE param;
	if (!param.Load(path))
	{
		error_output << "Can't find surfaces configfile: " << path << endl;
		return false;
	}
	
	list <string> sectionlist;
	param.GetSectionList(sectionlist);
	
	for (list<string>::const_iterator section = sectionlist.begin(); section != sectionlist.end(); ++section)
	{
		TRACKSURFACE surf;
		surf.name = *section;
		
		int id;
		param.GetParam(*section + ".ID", id);  // for sound..
		//-assert(indexnum >= 0 && indexnum < (int)tracksurfaces.size());
		surf.setType(id);
		
		float f = 0.f;
		param.GetParam(*section + ".BumpWaveLength", f, error_output);	surf.bumpWaveLength = f;
		param.GetParam(*section + ".BumpAmplitude", f, error_output);	surf.bumpAmplitude = f;
		if (param.GetParam(*section + ".BumpWaveLength2", f))  surf.bumpWaveLength2 = f;
		if (param.GetParam(*section + ".BumpAmplitude2", f))   surf.bumpAmplitude2 = f;
		
		param.GetParam(*section + ".FrictionTread", f, error_output);	surf.friction = f;
		if (param.GetParam(*section + ".FrictionX", f))   surf.frictionX = f;
		if (param.GetParam(*section + ".FrictionY", f))   surf.frictionY = f;
		
		if (param.GetParam(*section + ".RollResistance", f))			surf.rollingResist = f;
		param.GetParam(*section + ".RollingDrag", f, error_output);		surf.rollingDrag = f;


		///---  Tire  ---
		string tireFile;
		//if (!param.GetParam(*section + "." + "Tire", tireFile, error_output))
		if (!param.GetParam(*section + "." + "Tire", tireFile))
		{
			tireFile = track.sDefaultTire;  // default surface if not found
			//error_output << "Surface: Tire file not found, using default: " << tireFile << endl;
		}
		id = tires_map[tireFile]-1;
		if (id == -1)
		{	id = 0;
			error_output << "Surface: Tire id not found in map, using 0, " << tireFile << endl;
		}
		//error_output << "Tires size: " << pGame->tires.size() << endl;
		surf.tire = &tires[id];
		surf.tireName = tireFile;
		///---
		

		surfaces.push_back(surf);
		surf_map[surf.name] = (int)surfaces.size();  //+1, 0 = not found
	}
	return true;
}


///  Tires  all in data/carsim/normal/tires/*.tire
//------------------------------------------------------------------------------------------------------------------------------
bool GAME::LoadTire(CARTIRE& ct, string path, string& file)
{
	CONFIGFILE c;
	if (!c.Load(path+"/"+file))
	{	error_output << "Error loading tire file " << file << "\n";
		return false;
	}
	file = file.substr(0, file.length()-5);  // no ext .tire
	float value;

	for (int i = 0; i < 15; ++i)
	{
		int numinfile = i;
		if (i == 11)		numinfile = 111;
		else if (i == 12)	numinfile = 112;
		else if (i > 12)	numinfile -= 1;
		stringstream str;  str << "params.a" << numinfile;
		if (!c.GetParam(str.str(), value, error_output))  return false;
		ct.lateral[i] = value;
	}
	for (int i = 0; i < 11; ++i)
	{
		stringstream str;  str << "params.b" << i;
		if (!c.GetParam(str.str(), value, error_output))  return false;
		ct.longitudinal[i] = value;
	}
	for (int i = 0; i < 18; ++i)
	{
		stringstream str;  str << "params.c" << i;
		if (!c.GetParam(str.str(), value, error_output))  return false;
		ct.aligning[i] = value;
	}
	ct.name = file;
	ct.CalculateSigmaHatAlphaHat();
	return true;
}

bool GAME::LoadTires()
{
	tires.clear();
	tires_map.clear();
	
	//  load from both user and orig dirs
	for (int u=0; u < 2; ++u)
	{
		string path = u == 1 ? PATHMANAGER::CarSimU() : PATHMANAGER::CarSim();
		path += "/" + settings->game.sim_mode + "/tires";
		list <string> li;
		PATHMANAGER::DirList(path, li);

		for (list <string>::iterator i = li.begin(); i != li.end(); ++i)
		{
			string file = *i;
			if (file.find(".tire") != string::npos)
			{
				CARTIRE ct;
				ct.user = u;
				if (LoadTire(ct, path, file))
				{
					tires.push_back(ct);
					tires_map[file] = (int)tires.size();  //+1, 0 = not found
					TRACKSURFACE::pTireDefault = &ct;  //-
				}else
					LogO("Error Loading tire: "+file);
			}
	}	}
	return true;
}
CARTIRE* TRACKSURFACE::pTireDefault = 0;  //-

//  for graphs only
void GAME::PickTireRef(std::string name)
{
	tire_ref = name;
	int id = tires_map[name]-1;
	if (id == -1)
	{	id = 0;  info_output << "Reference tire not found: " << name << endl;  }
	tire_ref_id = id;
	/*if (!cars.empty())
		cars.begin()->GraphsNewVals(0.1);*/
}


///  Suspension factors
//------------------------------------------------------------------------------------------------------------------------------
bool GAME::LoadSusp()
{
	suspS.clear();  suspS_map.clear();
	suspD.clear();  suspD_map.clear();
	
	string path = PATHMANAGER::CarSim() + "/" + settings->game.sim_mode + "/susp";
	list <string> li;
	PATHMANAGER::DirList(path, li);
	for (list <string>::iterator i = li.begin(); i != li.end(); ++i)
	{
		string file = *i;
		if (file.find(".susp") != string::npos)
		{
			CONFIGFILE c;
			if (!c.Load(path+"/"+file))
			{	error_output << "Error loading susp file " << file << "\n";
				return false;  }

			file = file.substr(0, file.length()-5);

			//  factor points
			vector <pair <double, double> > damper, spring;
			c.GetPoints("suspension", "damper-factor", damper);
			c.GetPoints("suspension", "spring-factor", spring);

			suspS.push_back(spring);
			suspD.push_back(damper);
			suspS_map[file] = (int)suspS.size();  //+1, 0 = not found
			suspD_map[file] = (int)suspD.size();
		}
	}
	return true;
}
//------------------------------------------------------------------------------------------------------------------------------


bool GAME::InitializeSound()
{
	Ogre::Timer ti;
	int i;
	if (sound.Init(2048/*1024/*512*/, info_output, error_output))
	{
		sound_lib.SetLibraryPath(PATHMANAGER::Sounds());
		const SOUNDINFO & sdi = sound.GetDeviceInfo();

		#define Lsnd(n)   if (!sound_lib.Load(n,1,sdi, error_output))  return false
		#define Lsnd2(n,snd)  Lsnd(n);  \
			if (!snd.Setup(sound_lib, n,	 error_output,  false, false,1.f))  return false;  \
			sound.AddSource(snd);
		
		//  Load sounds ----
		Lsnd("tire_squeal");  Lsnd("grass");  Lsnd("gravel");
		
		Lsnd("bump_front");  Lsnd("bump_rear");
		Lsnd("wind");  Lsnd("boost");

		for (i = 1; i <= Ncrashsounds; ++i)
		{	std::string s = "crash/";  s += toStr(i/10)+toStr(i%10);
			Lsnd(s);
		}
		Lsnd("crash/scrap");
		Lsnd("crash/screech");

		for (i = 0; i < Nwatersounds; ++i)
			Lsnd("water"+toStr(i+1));

		Lsnd("mud1");  Lsnd("mud_cont");  Lsnd("water_cont");

		//  Hud 2d  ----
		Lsnd2("hud/check", snd_chk);
		Lsnd2("hud/check_wrong", snd_chkwr);

		Lsnd2("hud/lap", snd_lap);
		Lsnd2("hud/lap_best", snd_lapbest);

		Lsnd2("hud/stage", snd_stage);
		
		for (i = 0; i < 3; ++i)
		{	std::string s = "hud/win" + toStr(i);
			if (!sound_lib.Load(s,0,sdi, error_output))  return false;
			if (!snd_win[i].Setup(sound_lib, s,		error_output,  false, false,1.f))  return false;
			sound.AddSource(snd_win[i]);
		}
		Lsnd2("hud/fail", snd_fail);

		
		sound.SetMasterVolume(settings->vol_master);
		sound.Pause(false);
		UpdHudSndVol();

		info_output << "Sound initialization successful" << endl;
	}else
	{	error_output << "Sound initialization failed" << endl;
		return false;
	}

	info_output << "::: Time Sounds: " << fToStr(ti.getMilliseconds(),0,3) << " ms" << endl;
	return true;
}

void GAME::UpdHudSndVol()
{
	float g = settings->vol_hud;
	snd_chk.SetGain(g);  snd_chkwr.SetGain(g);
	snd_lap.SetGain(g);  snd_lapbest.SetGain(g);
	for (int i=0; i<3; ++i)  snd_win[i].SetGain(g);
	snd_fail.SetGain(g);
}



//  do any necessary cleanup
void GAME::End()
{
	if (benchmode)
	{
		float mean_fps = displayframe / clocktime;
		info_output << "Elapsed time: " << clocktime << " seconds\n";
		info_output << "Average frame-rate: " << mean_fps << " frames per second\n";
		info_output << "Min / Max frame-rate: " << fps_min << " / " << fps_max << " frames per second" << endl;
	}

	if (profilingmode)
		info_output << "Profiling summary:\n" << PROFILER.getSummary(quickprof::PERCENT) << endl;

	info_output << "Shutting down..." << endl;

	LeaveGame();

	if (sound.Enabled())
		sound.Pause(true); //stop the sound thread

	///+
	settings->Save(PATHMANAGER::SettingsFile()); //save settings first incase later deinits cause crashes

	collision.Clear();
	track.Clear();
}

void GAME::Test()
{
	QT_RUN_TESTS;

	info_output << endl;
}


///  the main game loop
//----------------------------------------------------------------------------------------------------------------------------
bool GAME::OneLoop(double dt)
{
	if (reloadSimNeed)
	{	// 	upd tweak tire save
		reloadSimNeed = false;
		ReloadSimData();
		reloadSimDone = true;
	}	

	PROFILER.beginBlock(" oneLoop");

	clocktime += dt;  //only for stats

	//LogO(Ogre::String("Ld: dt ")+fToStr(dt,6,8));

	Tick(dt);  // do CPU intensive stuff in parallel with the GPU

	displayframe++;  //only for stats

	PROFILER.endBlock(" oneLoop");
	return true;
}

///  step game required amount of ticks
void GAME::Tick(double deltat)
{
	const float minfps = 10.0f; //this is the minimum fps the game will run at before it starts slowing down time
	const unsigned int maxticks = (int) (1.0f / (minfps * framerate));
	const float maxtime = 1.0/minfps;
	unsigned int curticks = 0;

	//  throw away wall clock time if necessary to keep the framerate above the minimum
	if (deltat > maxtime)
		deltat = maxtime;
		
	//.  dont simulate before /network start
	bool sim = app->iLoad1stFrames == -2 && (!timer.waiting || timer.end_sim);

	//if (rand()%200 > 2)  sim = false;  // test start pos
	//LogO("SIM:"+fToStr(deltat,4,6) + (!sim ? "----":""));

	if (app && app->bPerfTest)  // speed up perf test
		deltat *= settings->perf_speed;
	
	target_time += deltat;
	double tickperriod = TickPeriod();

	//  increment game logic by however many tick periods have passed since the last GAME::Tick
	while (target_time > tickperriod && curticks < maxticks)
	{
		frame++;
		AdvanceGameLogic(sim ? tickperriod : 0.0);

		if (app)
			app->newPoses(tickperriod);

		curticks++;
		target_time -= tickperriod;
	}
}

///  simulate game by one frame
//----------------------------------------------------------------------------------------------------------------------------
void GAME::AdvanceGameLogic(double dt)
{
	if (track.Loaded())
	{
		if (pause && carcontrols_local.first)
			sound.Pause(true);
		else
		{
			if (sound.Enabled())
				sound.Pause(false);

			//PROFILER.beginBlock("ai");
			//ai.Visualize(rootnode);
			//ai.update(TickPeriod(), &track, cars); //-
			//PROFILER.endBlock("ai");

			PROFILER.beginBlock("-physics");
			///~~  clear fluids for each car
			for (list <CAR>::iterator i = cars.begin(); i != cars.end(); ++i)
			{
				(*i).dynamics.inFluids.clear();
				(*i).dynamics.velPrev = (*i).dynamics.chassis->getLinearVelocity();
				for (int w=0; w < 4; ++w)
					(*i).dynamics.inFluidsWh[w].clear();
			}

			if (dt > 0.0)
				collision.Update(dt, settings->bltProfilerTxt);
			PROFILER.endBlock("-physics");

			PROFILER.beginBlock("-car-sim");
			int i = 0;
			for (list <CAR>::iterator it = cars.begin(); it != cars.end(); ++it, ++i)
				UpdateCar(*it, TickPeriod());
			PROFILER.endBlock("-car-sim");

			//PROFILER.beginBlock("timer");
			UpdateTimer();
			//PROFILER.endBlock("timer");
		}
	}

	UpdateForceFeedback(TickPeriod());
}


///  send inputs to the car, check for collisions, and so on
//-----------------------------------------------------------
void GAME::UpdateCar(CAR & car, double dt)
{
	car.Update(dt);
	UpdateCarInputs(car);
	//UpdateDriftScore(car, dt);
}

void GAME::UpdateCarInputs(CAR & car)
{
	vector <float> carinputs(CARINPUT::ALL, 0.0f);
	//  race countdown or loading
	bool forceBrake = timer.waiting || timer.pretime > 0.f || app->iLoad1stFrames > -2;

	int i = app->scn->sc->asphalt ? 1 : 0;
	float sss_eff = settings->sss_effect[i], sss_velf = settings->sss_velfactor[i];
	float carspeed = car.GetSpeedDir();  //car.GetSpeed();
	//LogO(fToStr(car.GetSpeed(),2,6)+" "+fToStr(car.GetSpeedDir(),2,6));

	boost::lock_guard<boost::mutex> lock(app->input->mPlayerInputStateMutex);
	carinputs = carcontrols_local.second.ProcessInput(
		app->input->mPlayerInputState[car.id], car.id,
		carspeed, sss_eff, sss_velf,  app->mInputCtrlPlayer[car.id]->mbOneAxisThrottleBrake,
		forceBrake, app->bPerfTest, app->iPerfTestStage);

	car.HandleInputs(carinputs, TickPeriod());
}


bool GAME::NewGameDoCleanup()
{
	LeaveGame(); //this should clear out all data
	return true;
}

bool GAME::NewGameDoLoadTrack()
{
	if (!LoadTrack(settings->game.track))
		error_output << "Error during track loading: " << settings->game.track << endl;

	return true;
}

bool GAME::NewGameDoLoadMisc(float pre_time)
{
	//race_laps = num_laps;
	///-----
	race_laps = 0;

	opponents.clear();

	//send car sounds to the sound subsystem
	for (list <CAR>::iterator i = cars.begin(); i != cars.end(); ++i)
	{
		list <SOUNDSOURCE *> soundlist;
		i->GetSoundList(soundlist);
		for (list <SOUNDSOURCE *>::iterator s = soundlist.begin(); s != soundlist.end(); ++s)
			sound.AddSource(**s);
	}

	//load the timer
	if (!timer.Load(PATHMANAGER::Records()+"/"+ settings->game.sim_mode+"/"+ settings->game.track+".txt", pre_time, error_output))
		return false;

	//add cars to the timer system
	for (list <CAR>::iterator i = cars.begin(); i != cars.end(); ++i)
		timer.AddCar(i->GetCarType());
	timer.AddCar("ghost");

	return true;
}

///  clean up all game data
void GAME::LeaveGame()
{
	//ai.clear_cars();

	carcontrols_local.first = NULL;

	track.Unload();
	collision.Clear();

	if (sound.Enabled())
	{
		for (list <CAR>::iterator i = cars.begin(); i != cars.end(); ++i)
		{
			list <SOUNDSOURCE *> soundlist;
			i->GetSoundList(soundlist);
			for (list <SOUNDSOURCE *>::iterator s = soundlist.begin(); s != soundlist.end(); s++)
				sound.RemoveSource(*s);
		}
	}
	
	cars.clear();
	timer.Unload();
	pause = false;
}

///  add a car, optionally controlled by the local player
CAR* GAME::LoadCar(const string & pathCar, const string & carname, const MATHVECTOR<float,3> & start_position,
				   const QUATERNION<float> & start_orientation, bool islocal, bool isai,
				   bool isRemote, int idCar)
{
	CONFIGFILE carconf;
	if (!carconf.Load(pathCar))
		return NULL;

	cars.push_back(CAR());

	if (!cars.back().Load(app,
		carconf, carname,
		start_position, start_orientation,
		collision,
		sound.Enabled(), sound.GetDeviceInfo(), sound_lib,
		settings->abs || isai,
		settings->tcs || isai,
		isRemote, idCar,
		debugmode, info_output, error_output))
	{
		error_output << "Error loading car: " << carname << endl;
		cars.pop_back();
		return NULL;
	}
	else
	{
		info_output << "Car loaded: " << carname << endl;

		if (islocal)
		{
			//load local controls
			carcontrols_local.first = &cars.back();

			//setup auto clutch and auto shift
			ProcessNewSettings();
			// shift into first gear if autoshift enabled
			if (carcontrols_local.first && settings->autoshift)
				carcontrols_local.first->SetGear(1);
		}
	}
	return &cars.back();
}

bool GAME::LoadTrack(const string & trackname)
{
	LoadingScreen(0.0,1.0);

	//load the track
	if (!track.DeferredLoad(
		(settings->game.track_user ? PATHMANAGER::TracksUser() : PATHMANAGER::Tracks()) + "/" + trackname,
		settings->game.trackreverse,
		/**/0, "large", true, false))
	{
		error_output << "Error loading track: " << trackname << endl;
		return false;
	}
	bool success = true;
	int count = 0;
	while (!track.Loaded() && success)
	{
		int displayevery = track.DeferredLoadTotalObjects() / 50;
		if (displayevery == 0 || count % displayevery == 0)
		{
			LoadingScreen(count, track.DeferredLoadTotalObjects());
		}
		success = track.ContinueDeferredLoad();
		count++;
	}

	if (!success)
	{
		error_output << "Error loading track (deferred): " << trackname << endl;
		return false;
	}

	//setup track collision
	collision.SetTrack(&track);
	collision.DebugPrint(info_output);

	return true;
}

bool SortStringPairBySecond (const pair<string,string> & first, const pair<string,string> & second)
{
	return first.second < second.second;
}

void GAME::LoadSaveOptions(OPTION_ACTION action, map<string, string> & options)
{
	if (action == LOAD) //load from the settings class to the options map
	{
		CONFIGFILE tempconfig;
		settings->Serialize(true, tempconfig);
		list <string> paramlistoutput;
		tempconfig.GetParamList(paramlistoutput);
		for (list <string>::iterator i = paramlistoutput.begin(); i != paramlistoutput.end(); ++i)
		{
			string val;
			tempconfig.GetParam(*i, val);
			options[*i] = val;
			//cout << "LOAD - PARAM: " << *i << " = " << val << endl;
		}
	}
	else //save from the options map to the settings class
	{
		CONFIGFILE tempconfig;
		for (map<string, string>::iterator i = options.begin(); i != options.end(); ++i)
		{
			tempconfig.SetParam(i->first, i->second);
			//cout << "SAVE - PARAM: " << i->first << " = " << i->second << endl;
		}
		settings->Serialize(false, tempconfig);

		//account for new settings
		ProcessNewSettings();
	}
}

//  update the game with any new setting changes that have just been made
void GAME::ProcessNewSettings()
{
	if (carcontrols_local.first)
	{
		int i = app->scn->sc->asphalt ? 1 : 0;
		carcontrols_local.first->SetABS(settings->abs[i]);
		carcontrols_local.first->SetTCS(settings->tcs[i]);
		carcontrols_local.first->SetAutoShift(settings->autoshift);
		carcontrols_local.first->SetAutoRear(settings->autorear);
		//carcontrols_local.first->SetAutoClutch(settings->rear_inv);
	}
	sound.SetMasterVolume(settings->vol_master);
}

void GAME::LoadingScreen(float progress, float max)
{
	//assert(max > 0);
	//loadingscreen.Update(progress/(max+0.001f));	///+-

	//CollapseSceneToDrawlistmap(loadingscreen_node, graphics.GetDrawlistmap(), true);
}

void GAME::UpdateForceFeedback(float dt)
{
#ifdef ENABLE_FORCE_FEEDBACK
	if (carcontrols_local.first)
	{
		//static ofstream file("ff_output.txt");
		ff_update_time += dt;
		const double ffdt = 0.02;
		if (ff_update_time >= ffdt )
		{
			ff_update_time = 0.0;
			double feedback = -carcontrols_local.first->GetFeedback();

			feedback = settings->ff_gain * feedback / 100.0;
			if (settings->ff_invert) feedback = -feedback;

			if (feedback > 1.0)
				feedback = 1.0;
			if (feedback < -1.0)
				feedback = -1.0;
			//feedback += 0.5;
			/*
			static double motion_frequency = 0.1;
			static double motion_amplitude = 4.0;
			static double spring_strength = 1.0;
			*/
			//double center = sin( timefactor * 2 * M_PI * motion_frequency ) * motion_amplitude;
			double force = feedback;

			//cout << "ff_update_time: " << ff_update_time << " force: " << force << endl;
			forcefeedback->update(force, &feedback, ffdt, error_output);
		}
	}

	if (pause && dt == 0)
	{
		double pos=0;
		forcefeedback->update(0, &pos, 0.02, error_output);
	}
#endif
}

void GAME::UpdateDriftScore(CAR & car, double dt)
{
	bool is_drifting = false;
	bool spin_out = false;

	//make sure the car is not off track
	int wheel_count = 0;
	for (int i=0; i < 4; i++)
	{
		if ( car.GetCurPatch ( WHEEL_POSITION(i) ) ) wheel_count++;
	}

	bool on_track = ( wheel_count > 1 );
	int carId = 0;

	//car's direction on the horizontal plane
	MATHVECTOR<float,3> car_orientation(1,0,0);
	car.GetOrientation().RotateVector(car_orientation);
	car_orientation[2] = 0;

	//car's velocity on the horizontal plane
	MATHVECTOR<float,3> car_velocity = car.GetVelocity();
	car_velocity[2] = 0;
	float car_speed = car_velocity.Magnitude();

	//angle between car's direction and velocity
	float car_angle = 0;
	float mag = car_orientation.Magnitude() * car_velocity.Magnitude();
	if (mag > 0.001)
	{
		float dotprod = car_orientation.dot ( car_velocity )/mag;
		if (dotprod > 1.0)
			dotprod = 1.0;
		if (dotprod < -1.0)
			dotprod = -1.0;
		car_angle = acos(dotprod);
	}

	assert(car_angle == car_angle); //assert that car_angle isn't NAN

	if ( on_track )
	{
		//velocity must be above 10 m/s
		if ( car_speed > 10 )
		{
			//drift starts when the angle > 0.2 (around 11.5 degrees)
			//drift ends when the angle < 0.1 (aournd 5.7 degrees)
			float angle_threshold(0.2);
			if ( timer.GetIsDrifting(carId) ) angle_threshold = 0.1;

			is_drifting = ( car_angle > angle_threshold && car_angle <= M_PI/2.0 );
			spin_out = ( car_angle > M_PI/2.0 );
		}
	}

	//calculate score
	if ( is_drifting )
	{
		//base score is the drift distance
		timer.IncrementThisDriftScore(carId, dt * car_speed);

		//bonus score calculation is now done in TIMER
		timer.UpdateMaxDriftAngleSpeed(carId, car_angle, car_speed);

		//cout << timer.GetDriftScore(carId) << " + " << timer.GetThisDriftScore(carId) << endl;
	}

	if (settings->multi_thr != 1)
		timer.SetIsDrifting(carId, is_drifting, on_track && !spin_out);

	//cout << is_drifting << ", " << on_track << ", " << car_angle << endl;
}


//  break up the input into a vector of strings using the token characters given
vector <string> Tokenize(const string & input, const string & tokens)
{
	vector <string> out;

	unsigned int pos = 0;
	unsigned int lastpos = 0;

	while (pos != (unsigned int) string::npos)
	{
		pos = input.find_first_of(tokens, pos);
		string thisstr = input.substr(lastpos,pos-lastpos);
		if (!thisstr.empty())
			out.push_back(thisstr);
		pos = input.find_first_not_of(tokens, pos);
		lastpos = pos;
	}

	return out;
}

bool GAME::ParseArguments(list <string> & args)
{
	bool continue_game(true);

	map <string, string> arghelp;
	map <string, string> argmap;

	//generate an argument map
	for (list <string>::iterator i = args.begin(); i != args.end(); ++i)
	{
		if ((*i)[0] == '-')
		{
			argmap[*i] = "";
		}

		list <string>::iterator n = i;
		n++;
		if (n != args.end())
		{
			if ((*n)[0] != '-')
				argmap[*i] = *n;
		}
	}

	//check for arguments

	if (argmap.find("-test") != argmap.end())
	{
		Test();
		continue_game = false;
	}
	arghelp["-test"] = "Run unit tests.";

	if (argmap.find("-debug") != argmap.end())
	{
		debugmode = true;
	}
	///+
	//debugmode = true;
	arghelp["-debug"] = "Display car debugging information.";

	//if (!argmap["-cartest"].empty())
	{
		//PATHMANAGER::Init(info_output, error_output);
		//PERFORMANCE_TESTING perftest;
		//perftest.Test(PATHMANAGER::GetCarPath(), , info_output, error_output);
		//continue_game = false;
	}
	arghelp["-cartest CAR"] = "Run car performance testing on given CAR.";

	///+
	//if (argmap.find("-profiling") != argmap.end() || argmap.find("-benchmark") != argmap.end())
	//if (settings->bltLines/*bltProfilerTxt*/)
	{
		PROFILER.init(20);
		profilingmode = true;
	}
	arghelp["-profiling"] = "Display game performance data.";

	if (argmap.find("-dumpfps") != argmap.end())
	{
		info_output << "Dumping the frame-rate to log." << endl;
		dumpfps = true;
	}
	arghelp["-dumpfps"] = "Continually dump the framerate to the log.";


	if (argmap.find("-nosound") != argmap.end())
		sound.DisableAllSound();
	arghelp["-nosound"] = "Disable all sound.";

	if (argmap.find("-benchmark") != argmap.end())
	{
		info_output << "Entering benchmark mode." << endl;
		benchmode = true;
	}
	arghelp["-benchmark"] = "Run in benchmark mode.";


	arghelp["-help"] = "Display command-line help.";
	if (argmap.find("-help") != argmap.end() || argmap.find("-h") != argmap.end() || argmap.find("--help") != argmap.end() || argmap.find("-?") != argmap.end())
	{
		string helpstr;
		unsigned int longest = 0;
		for (map <string,string>::iterator i = arghelp.begin(); i != arghelp.end(); ++i)
			if (i->first.size() > longest)
				longest = i->first.size();
		for (map <string,string>::iterator i = arghelp.begin(); i != arghelp.end(); ++i)
		{
			helpstr.append(i->first);
			for (unsigned int n = 0; n < longest+3-i->first.size(); n++)
				helpstr.push_back(' ');
			helpstr.append(i->second + "\n");
		}
		info_output << "Command-line help:\n\n" << helpstr << endl;
		continue_game = false;
	}

	return continue_game;
}


void GAME::UpdateTimer()
{
	if (app->iLoad1stFrames == -2)  // ended loading
		timer.Tick(TickPeriod());
	//timer.DebugPrint(info_output);
}


///  Car Steering range multiplier
float GAME::GetSteerRange() const
{
	float range = settings->steer_sim[settings->gui.sim_mode == "easy" ? 0 : 1];
	range *= settings->steer_range[track.asphalt];
	return range;
}
void* GAME::custom_duty(void){
    void *nothing;
    int dirty=1;
    zmq::context_t context (1);
    zmq::socket_t socket (context, ZMQ_REP);
    socket.bind ("tcp://*:5555");
    zmq::message_t request;
    while (true) {
        if(!cars.empty()){
            // Wait for next request from client
            socket.recv (&request);
            std::cout << "Received Hello" << std::endl;
            std::string rpl = std::string(static_cast<char*>(request.data()), request.size());
            // Do some 'work'
            //sleep(1);
            // Send reply back to client
            zmq::message_t reply (2);
            memcpy ((void *) reply.data (), "OK", 2);
            socket.send (reply);
            for (list <CAR>::iterator car = cars.begin(); car != cars.end(); ++car)
            {
                //car->dynamics.boostVal = settings->game.boost_max;
                //car->dynamics.doBoost = 1;
                //car->SetTCS(true);
                //car->SetABS(true);
                if(!rpl.compare("brake100")){
                    car->dynamics.SetBrake(100);                
                }else if(!rpl.compare("brake0")){
                    car->dynamics.SetBrake(0);                
                }
                else if(!rpl.compare("boostmax")){
                    car->dynamics.boostFuel = settings->game.boost_max;               
                }
                else if(!rpl.compare("boost2")){
                    car->dynamics.boostFuel = car->dynamics.boostFuel<settings->game.boost_max?car->dynamics.boostFuel+2:settings->game.boost_max;               
                }
                break;
            }
        }
        sleep(1);    
    }
    return nothing;
}
void* GAME::custom_duty_helper(void *context)
    {
        return ((GAME *)context)->custom_duty();
    }

