// Copyright 2019 Google LLC & Bastiaan Konings
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// written by bastiaan konings schuiling 2008 - 2015
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#include "match.hpp"

#include <algorithm>
#include <cmath>

#include "../base/geometry/triangle.hpp"
#include "../base/log.hpp"
#include "../main.hpp"
#include "../menu/pagefactory.hpp"
#include "../menu/startmatch/loadingmatch.hpp"
#include "../scene/objectfactory.hpp"
#include "../scene/objects/light.hpp"
#include "../utils/splitgeometry.hpp"
#include "AIsupport/AIfunctions.hpp"
#include "file.h"
#include "player/playerofficial.hpp"
#include "proceduralpitch.hpp"

constexpr unsigned int replaySize_ms = 10000;
constexpr unsigned int camPosSize = 150;

boost::shared_ptr<AnimCollection> Match::GetAnimCollection() { return GetContext().anims; }

const std::vector<Vector3> &Match::GetAnimPositionCache(Animation *anim) const {
  return GetContext().animPositionCache.find(anim)->second;
}

Match::Match(MatchData *matchData, const std::vector<IHIDevice *> &controllers)
    : matchData(matchData),
      first_team(GetScenarioConfig().reverse_team_processing ? 1 : 0),
      second_team(GetScenarioConfig().reverse_team_processing ? 0 : 1),
      controllers(controllers),
      possessionSideHistory(6000),
      matchDurationFactor(
          GetConfiguration()->GetReal("match_duration", 1.0) * 0.2f + 0.05f),
      _useMagnet(GetScenarioConfig().use_magnet) {
  auto& anims = GetContext().anims;
  GetContext().stablePlayerCount = 0;

  // shared ptr to menutask, because menutask shouldn't die before match does
  menuTask = GetMenuTask();

  iterations = 0;
  actualTime_ms = 0;
  goalScoredTimer = 0;

  resetNetting = false;
  nettingHasChanged = false;

  dynamicNode = boost::intrusive_ptr<Node>(new Node("dynamicNode"));
  GetScene3D()->AddNode(dynamicNode);

  ball = new Ball(this);

  if (!anims) {
    anims = boost::shared_ptr<AnimCollection>(new AnimCollection());
    anims->Load();
    // cache animation positions

    const std::vector < Animation* > &animationsTmp = anims->GetAnimations();
    for (unsigned int i = 0; i < animationsTmp.size(); i++) {
      std::vector<Vector3> positions;
      Animation *someAnim = animationsTmp[i];
      Quaternion dud;
      Vector3 position;
      for (int frame = 0; frame < someAnim->GetFrameCount(); frame++) {
        someAnim->GetKeyFrame(player, frame, dud, position);
        position.coords[2] = 0.0f;
        positions.push_back(position);
      }
      GetContext().animPositionCache.insert(std::pair < Animation*, std::vector<Vector3> >(someAnim, positions));
    }
    GetVertexColors(GetContext().colorCoords);
  }


  // full body model template

  ObjectLoader loader;
  fullbodyNode = loader.LoadObject("media/objects/players/fullbody.object");

  designatedPossessionPlayer = 0;


  // teams

  assert(matchData != 0);

  teams[first_team] =
      new Team(first_team, this, &matchData->GetTeamData(first_team),
          GetScenarioConfig().left_team_difficulty,
          GetScenarioConfig().symmetric_mode || first_team == 0 ? -1 : 1);
  teams[second_team] =
      new Team(second_team, this, &matchData->GetTeamData(second_team),
          GetScenarioConfig().right_team_difficulty,
          GetScenarioConfig().symmetric_mode || second_team == 0 ? -1 : 1);
  teams[first_team]->SetOpponent(teams[second_team]);
  teams[second_team]->SetOpponent(teams[first_team]);
  teams[first_team]->InitPlayers(fullbodyNode, GetContext().colorCoords);
  teams[second_team]->InitPlayers(fullbodyNode, GetContext().colorCoords);

  std::vector<Player*> activePlayers;
  teams[first_team]->GetActivePlayers(activePlayers);
  designatedPossessionPlayer = activePlayers.at(0);
  ballRetainer = 0;


  // officials

  std::string kitFilename = "media/objects/players/textures/referee_kit.png";
  boost::intrusive_ptr<Resource<Surface> > kit =
      GetContext().surface_manager.Fetch(kitFilename);
  officials = new Officials(this, fullbodyNode, GetContext().colorCoords, kit, anims);

  dynamicNode->AddObject(officials->GetYellowCardGeom());
  dynamicNode->AddObject(officials->GetRedCardGeom());


  // camera

  camera = static_pointer_cast<Camera>(
      GetContext().object_factory.CreateObject("camera", e_ObjectType_Camera));
  GetScene3D()->CreateSystemObjects(camera);
  camera->Init();

  camera->SetFOV(25);
  cameraNode = boost::intrusive_ptr<Node>(new Node("cameraNode"));
  cameraNode->AddObject(camera);
  cameraNode->SetPosition(Vector3(40, 0, 100));
  GetDynamicNode()->AddNode(cameraNode);

  autoUpdateIngameCamera = true;


  // stadium
  boost::intrusive_ptr<Node> tmpStadiumNode;
  if (GetScenarioConfig().render) {
    tmpStadiumNode = loader.LoadObject("media/objects/stadiums/test/test.object");
    RandomizeAdboards(tmpStadiumNode);
  } else {
    tmpStadiumNode = loader.LoadObject("media/objects/stadiums/test/pitchonly.object");
  }
  std::list < boost::intrusive_ptr<Geometry> > stadiumGeoms;

  // split stadium geometry into multiple geometry objects, for more efficient culling
  tmpStadiumNode->GetObjects<Geometry>(e_ObjectType_Geometry, stadiumGeoms);
  assert(stadiumGeoms.size() != 0);

  stadiumNode = boost::intrusive_ptr<Node>(new Node("stadium"));

  std::list < boost::intrusive_ptr<Geometry> >::iterator iter = stadiumGeoms.begin();
  while (iter != stadiumGeoms.end()) {
    boost::intrusive_ptr<Node> tmpNode = SplitGeometry(GetScene3D(), *iter, 24);
    tmpNode->SetLocalMode(e_LocalMode_Absolute);
    stadiumNode->AddNode(tmpNode);

    iter++;
  }
  tmpStadiumNode->Exit();
  tmpStadiumNode.reset();

  stadiumNode->SetLocalMode(e_LocalMode_Absolute);
  GetScene3D()->AddNode(stadiumNode);


  // goal netting
  goalsNode = loader.LoadObject("media/objects/stadiums/goals.object");
  goalsNode->SetLocalMode(e_LocalMode_Absolute);
  GetScene3D()->AddNode(goalsNode);
  PrepareGoalNetting();


  // pitch
  if (GetGameConfig().high_quality) {
    GeneratePitch(2048, 1024, 1024, 512, 2048, 1024);
  } else {
    GeneratePitch(1024, 512, 1024, 512, 2048, 1024);
  }


  // sun
  sunNode = loader.LoadObject("media/objects/lighting/generic.object");
  GetDynamicNode()->AddNode(sunNode);
  SetRandomSunParams();


  // human gamers
  UpdateControllerSetup();


  // 12th man sound

  // match params

  matchTime_ms = 0;
  lastGoalTeam = 0;
  for (unsigned int i = 0; i < e_TouchType_SIZE; i++) {
    lastTouchTeamIDs[i] = -1;
  }
  lastTouchTeamID = -1;
  lastGoalScorer = 0;
  bestPossessionTeam = 0;
  SetMatchPhase(e_MatchPhase_PreMatch);

  // everybody hates him, this poor bloke
  referee = new Referee(this);


  // GUI
  Gui2Root *root = menuTask->GetWindowManager()->GetRoot();

  radar = new Gui2Radar(menuTask->GetWindowManager(), "game_radar", 38, 78, 24, 18, this, matchData->GetTeamData(0).GetColor1(), matchData->GetTeamData(0).GetColor2(), matchData->GetTeamData(1).GetColor1(), matchData->GetTeamData(1).GetColor2());
  root->AddView(radar);
  radar->Show();

  scoreboard = new Gui2ScoreBoard(menuTask->GetWindowManager(), this);
  root->AddView(scoreboard);
  scoreboard->Show();

  messageCaption = new Gui2Caption(menuTask->GetWindowManager(), "game_messages", 0, 0, 80, 8, "");
  messageCaption->SetTransparency(0.3f);
  root->AddView(messageCaption);
  messageCaptionRemoveTime_ms = actualTime_ms + 5000;

  // for usage in destructor
  scene3D = GetScene3D();

  lastBodyBallCollisionTime_ms = 0;

 sig_OnCreatedMatch(this);
  LoadingMatchPage *loadingMatchPage = static_cast<LoadingMatchPage*>(menuTask->GetWindowManager()->GetPageFactory()->GetMostRecentlyCreatedPage());
  loadingMatchPage->Close();
}

Match::~Match() {
}

void Match::MaybeMirror(bool team_0, bool team_1, bool ball) {
  if (GetScenarioConfig().symmetric_mode) {
    if (team_0) {
      team_0_mirrored = !team_0_mirrored;
      teams[0]->Mirror();
    }
    if (team_1) {
      team_1_mirrored = !team_1_mirrored;
      teams[1]->Mirror();
    }
    if (ball) {
      ball_mirrored = !ball_mirrored;
      this->ball->Mirror();
    }
    for (auto& i : mentalImages) {
      i.Mirror(team_0, team_1, ball);
    }
  }
}

void Match::Exit() {
  teams[first_team]->Exit();
  teams[second_team]->Exit();
  delete teams[first_team];
  delete teams[second_team];
  delete officials;
  delete ball;
  delete referee;
  delete matchData;
  menuTask->SetMatchData(0);
  mentalImages.clear();

  fullbodyNode->Exit();
  fullbodyNode.reset();

  messageCaption->Exit();
  delete messageCaption;

  scene3D->DeleteNode(GetDynamicNode());
  scene3D->DeleteNode(stadiumNode);
  scene3D->DeleteNode(goalsNode);
  radar->Exit();
  delete radar;

  scoreboard->Exit();
  delete scoreboard;

  menuTask.reset();
  sig_OnExitedMatch(this);
}

void Match::SetRandomSunParams() {

  float brightness = 1.0f;

  Vector3 sunPos = Vector3(-1.2f, 0.4f, 1.0f); // sane default
  float averageHeightMultiplier = 1.3f;
  sunPos = Vector3(clamp(boostrandom(-1.7f, 1.7f), -1.0, 1.0),
                   clamp(boostrandom(-1.7f, 1.7f), -1.0, 1.0),
                   averageHeightMultiplier);
  sunPos.Normalize();
  if (boostrandom(0, 1) > 0.5f && sunPos.coords[1] > 0.25f)
    sunPos.coords[1] =
        -sunPos.coords[1];  // sun more often on (default) camera side (coming
                            // from front == clearer lighting on players)
  sunNode->GetObject("sun")->SetPosition(sunPos * 10000.0f);

  float defaultRadius = 1000000.0f;
  float sunRadius = defaultRadius;
  static_pointer_cast<Light>(sunNode->GetObject("sun"))->SetRadius(sunRadius);

  Vector3 sunColorNoon(0.9, 0.8, 1.0); sunColorNoon *= 1.4f;
  Vector3 sunColorDusk(1.4, 0.9, 0.7); sunColorDusk *= 1.2f;

  float noonBias =
      std::pow(NormalizedClamp(sunPos.coords[2], 0.5f, 1.0f), 1.2f);
  Vector3 sunColor = sunColorNoon * noonBias + sunColorDusk * (1.0f - noonBias);

  Vector3 randomAddition(boostrandom(-0.1, 0.1), boostrandom(-0.1, 0.1),
                         boostrandom(-0.1, 0.1));
  randomAddition *= 1.2f;
  sunColor += randomAddition;

  static_pointer_cast<Light>(sunNode->GetObject("sun"))->SetColor(sunColor * brightness);
}

void Match::RandomizeAdboards(boost::intrusive_ptr<Node> stadiumNode) {
  // collect texture files

  std::vector<std::string> files;
  GetFiles("media/textures/adboards", "bmp", files);
  sort(files.begin(), files.end());

  std::vector < boost::intrusive_ptr < Resource<Surface> > > adboardSurfaces;
  for (unsigned int i = 0; i < files.size(); i++) {
    adboardSurfaces.push_back(GetContext().surface_manager.Fetch(files[i]));
  }
  if (adboardSurfaces.empty()) return;


  // collect adboard geoms

  std::list < boost::intrusive_ptr<Geometry> > stadiumGeoms;
  stadiumNode->GetObjects<Geometry>(e_ObjectType_Geometry, stadiumGeoms, true);
  // replace

  std::list < boost::intrusive_ptr<Geometry> >::const_iterator stadiumGeomsIter = stadiumGeoms.begin();
  while (stadiumGeomsIter != stadiumGeoms.end()) {

    boost::intrusive_ptr<Geometry> geomObject = *stadiumGeomsIter;
    assert(geomObject != boost::intrusive_ptr<Object>());
    boost::intrusive_ptr< Resource<GeometryData> > adboardGeom = geomObject->GetGeometryData();

    std::vector < MaterializedTriangleMesh > &tmesh = adboardGeom->GetResource()->GetTriangleMeshesRef();

    for (unsigned int i = 0; i < tmesh.size(); i++) {
      if (tmesh[i].material.diffuseTexture != boost::intrusive_ptr< Resource<Surface> >()) {
        std::string identString = tmesh[i].material.diffuseTexture->GetIdentString();
        //printf("%s\n", identString.c_str());
        if (identString.find("ad_placeholder") == 0) {
          tmesh[i].material.diffuseTexture = adboardSurfaces.at(
              int(std::floor(random_non_determ(0, adboardSurfaces.size() - 1.001f))));
          tmesh[i].material.specular_amount = 0.2f;
          tmesh[i].material.shininess = 0.1f;
        }
      }
    }

    geomObject->OnUpdateGeometryData();

    stadiumGeomsIter++;
  }

}

void Match::UpdateControllerSetup() {

  // remove current gamers
  teams[first_team]->DeleteHumanGamers();
  teams[second_team]->DeleteHumanGamers();

  // add new
  const std::vector<SideSelection> sides = menuTask->GetControllerSetup();
  for (unsigned int i = 0; i < sides.size(); i++) {
    if (sides[i].side == -1) {
      teams[0]->AddHumanGamer(controllers.at(sides[i].controllerID),
                              (e_PlayerColor)i);
    } else if (sides[i].side == 1) {
      teams[1]->AddHumanGamer(controllers.at(sides[i].controllerID),
                              (e_PlayerColor)i);
    }
  }
}

void Match::SpamMessage(const std::string &msg, int time_ms) {
  messageCaption->SetCaption(msg);
  float w = messageCaption->GetTextWidthPercent();
  messageCaption->SetPosition(50 - w * 0.5f, 5);
  messageCaption->Show();
  messageCaptionRemoveTime_ms = actualTime_ms + time_ms;
}

void Match::GetAllTeamPlayers(int teamID, std::vector<Player*> &players) {
  teams[teamID]->GetAllPlayers(players);
}

void Match::GetActiveTeamPlayers(int teamID, std::vector<Player*> &players) {
  teams[teamID]->GetActivePlayers(players);
}

void Match::GetOfficialPlayers(std::vector<PlayerBase*> &players) {
  officials->GetPlayers(players);
}

MentalImage* Match::GetMentalImage(int history_ms) {
  int index = int(round((float)history_ms / 10.0));
  if (index >= (signed int)mentalImages.size()) index = mentalImages.size() - 1;
  if (index < 0) index = 0;
  return &mentalImages[index];
}

void Match::UpdateLatestMentalImageBallPredictions() {
  if (!mentalImages.empty()) mentalImages[0].UpdateBallPredictions();
}

void Match::ResetSituation(const Vector3 &focusPos) {
  camPos.clear();
  SetBallRetainer(0);
  SetGoalScored(false);
  mentalImages.clear();
  goalScored = false;
  ballIsInGoal = false;
  for (unsigned int i = 0; i < e_TouchType_SIZE; i++) {
    lastTouchTeamIDs[i] = -1;
  }
  lastTouchTeamID = -1;
  lastGoalScorer = 0;
  bestPossessionTeam = 0;

  possessionSideHistory.Clear();

  lastBodyBallCollisionTime_ms = 0;

  ball->ResetSituation(focusPos);
  teams[first_team]->ResetSituation(focusPos);
  teams[second_team]->ResetSituation(focusPos);
}

void Match::SetMatchPhase(e_MatchPhase newMatchPhase) {
  matchPhase = newMatchPhase;
  if (matchPhase == e_MatchPhase_1stHalf) {
    teams[first_team]->RelaxFatigue(1.0f);
    teams[second_team]->RelaxFatigue(1.0f);
  }
}

Team* Match::GetBestPossessionTeam() {
  return bestPossessionTeam;
}

void Match::UpdateIngameCamera() {
  // camera

  float fov = 0.0f;
  float zoom = 0.0f;
  float height = 0.0f;

  fov = 0.5f + _default_CameraFOV * 0.5f;
  zoom = _default_CameraZoom;
  height = _default_CameraHeight * 1.5f;

  float playerBias = 0.6f;//0.7f;
  Vector3 ballPos = ball->Predict(0) * (1.0f - playerBias) + GetDesignatedPossessionPlayer()->GetPosition() * playerBias;
  // look in possession player's direction
  ballPos += GetDesignatedPossessionPlayer()->GetDirectionVec() * 1.0f;
  // look in possession team's attacking direction
  ballPos +=
      Vector3(((teams[first_team]->GetFadingTeamPossessionAmount() - 1.0f) *
                   -teams[first_team]->GetSide() +
               (teams[second_team]->GetFadingTeamPossessionAmount() - 1.0f) *
                   -teams[second_team]->GetSide()) *
                  4.0f,
              0, 0);

  ballPos.coords[2] *= 0.1f;

  float maxW = pitchHalfW * 0.84f * (1.0 / (zoom + 0.01f));// * (height * 0.75f + 0.25f);
  float maxH = pitchHalfH * 0.60f * (1.0 / (zoom + 0.01f)) * (height * 0.75f + 0.25f); // 0.52f
  if (fabs(ballPos.coords[0]) > maxW) ballPos.coords[0] = maxW * signSide(ballPos.coords[0]);
  if (fabs(ballPos.coords[1]) > maxH) ballPos.coords[1] = maxH * signSide(ballPos.coords[1]);

  Vector3 shudder = Vector3(random_non_determ(-0.1f, 0.1f), random_non_determ(-0.1f, 0.1f), 0) * (ball->GetMovement().GetLength() * 0.8f + 6.0f);
  shudder *= 0.2f;
  camPos.push_back(ballPos + shudder * ((float)camPos.size() / (float)camPosSize));
  if (camPos.size() > camPosSize) camPos.pop_front();

  Vector3 average;
  std::deque<Vector3>::iterator camIter = camPos.begin();
  float count = 0;
  float indexSize = camPos.size();
  int index = 0;
  while (camIter != camPos.end()) {
    float weight = std::sin((index / indexSize - 0.3f) * 1.4f * pi) * 0.5f +
                   0.5f;  // healthy mix of latest & middle | wa: sin((x / 100 -
                          // 0.3) * 1.4 * pi) * 0.5 + 0.5 | from x = 0 to 100
    weight *= std::pow(
        1.0f - index / indexSize,
        0.3f);  // sharp cutoff @ latest (because cameraperson can't 'foresee'
                // the current moment that fast) | wa: (1.0 - x / 100) ^ 0.3 *
                // (<prev formula>) | from x = 0 to 100
    average += (*camIter) * weight;
    count += weight;
    camIter++;
    index++;
  }

  average /= count;

  radian angleFac = 1.0f - _default_CameraAngleFactor * 0.4f; // 0.0 == 90 degrees max, 1.0 == sideline view

  // normal cam

  int camMethod = 1; // 1 == wide, 2 == birds-eye, 3 == tele

  if (!IsGoalScored() || (IsGoalScored() && goalScoredTimer < 1000)) {

    if (camMethod == 1) {

      // wide cam

      zoom = (0.6f + zoom * 1.0f) * (1.0f / fov);
      height = 4.0f + height * 10;

      float distRot = average.coords[1] / 800.0f;

      cameraOrientation.SetAngleAxis(distRot + (0.42f - height * 0.01f) * pi, Vector3(1, 0, 0));
      cameraNodeOrientation.SetAngleAxis((-average.coords[0] / pitchHalfW) * (1.0f - angleFac) * 0.25f * pi * 1.24f, Vector3(0, 0, 1));
      cameraNodePosition =
          average * Vector3(1.0f * (1.0f - _default_CameraAngleFactor * 0.2f) *
                                (1.0f - _default_CameraZoom * 0.3f),
                            0.9f - _default_CameraZoom * 0.3f, 0.2f) +
          Vector3(
              0,
              -41.4f - (_default_CameraFOV * 3.7f) + std::pow(height, 1.2f) * 0.46f,
              10.0f + height) *
              zoom;
      cameraFOV = (fov * 28.0f) - (cameraNodePosition.coords[1] / 30.0f);
      cameraNearCap = cameraNodePosition.coords[2];
      cameraFarCap = 200;

    } else if (camMethod == 2) {

      // birds-eye cam

      cameraOrientation = QUATERNION_IDENTITY;
      cameraNodeOrientation = QUATERNION_IDENTITY;
      cameraNodePosition = average * Vector3(1, 1, 0) + Vector3(0, 0, 50 + zoom * 20.0);
      cameraFOV = 28;
      cameraNearCap = 40 + height - 5;
      cameraFarCap = 250;//65 + height * 1.2; doesn't work wtf?

    } else if (camMethod == 3) {

      // tele cam

      zoom = (0.6f + zoom * 1.0f) * (1.0f / fov);

      cameraOrientation.SetAngleAxis(0.3f * pi * height + 0.4f * pi * (1.0 - height), Vector3(1, 0, 0));
      cameraNodeOrientation = QUATERNION_IDENTITY;
      Vector3 offset = Vector3(0, -175.0f, 125.0f) * height + Vector3(0, -230.0f, 65.0f) * (1.0 - height);
      cameraNodePosition = average * Vector3(0.9f, 0.7f, 0.2f) + offset * zoom * 0.4f;
      cameraFOV = 15.0f;
      cameraNearCap = 50 + zoom * 10.0f;
      cameraFarCap = 300;

    }

  } else {

    // scorer cam

    Vector3 targetPos = ball->Predict(0).Get2D();
    if (lastGoalScorer) {
      targetPos = lastGoalScorer->GetPosition();
    }

    radian rot = (float)goalScoredTimer * 0.0005f;
    cameraOrientation.SetAngleAxis(0.45f * pi, Vector3(1, 0, 0));
    cameraNodeOrientation.SetAngleAxis(rot, Vector3(0, 0, 1));
    cameraNodePosition = targetPos + Vector3(0, -1, 0).GetRotated2D(rot) * 15.0f + Vector3(0, 0, 3);
    cameraFOV = 35.0f;

    cameraNearCap = 1;
    cameraFarCap = 220;
  }
}

void Match::ProcessState(EnvState* state) {
  bool team_0_mirror = team_0_mirrored;
  bool team_1_mirror = team_1_mirrored;
  bool ball_mirror = ball_mirrored ^ GetScenarioConfig().reverse_team_processing;
  MaybeMirror(team_0_mirror, team_1_mirror, ball_mirror);
  std::vector<Player*> players;
  GetAllTeamPlayers(first_team, players);
  GetAllTeamPlayers(second_team, players);
  std::vector<HumanController*> humanControllers;
  teams[first_team]->GetHumanControllers(humanControllers);
  teams[second_team]->GetHumanControllers(humanControllers);
  state->SetHumanControllers(humanControllers);
  state->SetPlayers(players);
  state->SetAnimations(GetContext().anims->GetAnimations());
  state->SetTeams(teams[first_team], teams[second_team]);

  int size = mentalImages.size();
  state->process(size);
  mentalImages.resize(size);
  for (int x = 0; x < size; x++) {
    mentalImages[x].ProcessState(state, this);
  }
  for (auto &player : players) {
    player->ProcessState(state);
  }
  matchData->ProcessState(state, first_team);
  teams[first_team]->ProcessState(state);
  teams[second_team]->ProcessState(state);
  officials->ProcessState(state);
  state->setValidate(false);
  for (auto& c : controllers) {
    c->ProcessState(state);
  }
  state->setValidate(true);
  ball->ProcessState(state);
  state->process(iterations);
  state->process(matchTime_ms);
  state->process(actualTime_ms);
  state->process(goalScoredTimer);
  state->process(pause);
  state->process((void*) &matchPhase, sizeof(e_MatchPhase));
  state->process(inPlay);
  state->process(inSetPiece);
  state->process(goalScored);
  state->process(ballIsInGoal);
  state->process(lastGoalTeam);
  state->process(lastGoalScorer);
  if (first_team == 1) {
    for (int &v : lastTouchTeamIDs) {
      if (v != -1) {
        v = 1 - v;
      }
    }
  }
  for (int& v : lastTouchTeamIDs) {
    state->process(v);
  }
  if (first_team == 1) {
    for (int &v : lastTouchTeamIDs) {
      if (v != -1) {
        v = 1 - v;
      }
    }
  }
  if (first_team == 1 && lastTouchTeamID != -1) {
    lastTouchTeamID = 1 - lastTouchTeamID;
  }
  state->process(lastTouchTeamID);
  if (first_team == 1 && lastTouchTeamID != -1) {
    lastTouchTeamID = 1 - lastTouchTeamID;
  }
  state->process(bestPossessionTeam);
  state->process(designatedPossessionPlayer);
  state->process(ballRetainer);
  possessionSideHistory.ProcessState(state);
  state->process(autoUpdateIngameCamera);
  state->setValidate(false);
  state->process(cameraOrientation);
  state->process(cameraNodeOrientation);
  state->process(cameraNodePosition);
  state->process(cameraFOV);
  state->process(cameraNearCap);
  state->process(cameraFarCap);
  state->process(lastBodyBallCollisionTime_ms);
  size = camPos.size();
  state->process(size);
  camPos.resize(size);
  for (auto& v : camPos) {
    state->process(v);
  }
  state->setValidate(true);
  referee->ProcessState(state);

  if (state->Load()) {
    resetNetting = true;
    nettingHasChanged = true;
  }
  MaybeMirror(team_0_mirror, team_1_mirror, ball_mirror);
}

void Match::GetState(SharedInfo *state) {
  state->ball_position = ball->GetAveragePosition(5).coords;
  state->ball_rotation =
      (ball->GetRotation() / GetGameConfig().physics_steps_per_frame).coords;
  state->ball_direction =
      (ball->GetMovement() / GetGameConfig().physics_steps_per_frame).coords;
  state->ball_owned_player = -1;
  state->ball_owned_team = -1;
  state->left_goals = GetScore(0);
  state->right_goals = GetScore(1);
  state->is_in_play = IsInPlay();
  state->game_mode = IsInSetPiece() ? referee->GetBuffer().desiredSetPiece : e_GameMode_Normal;
  state->left_controllers.clear();
  state->left_controllers.resize(GetScenarioConfig().left_team.size());
  state->right_controllers.clear();
  state->right_controllers.resize(GetScenarioConfig().right_team.size());

  std::map<IHIDevice*, int> controller_mapping;
  {
    auto controllers = GetControllers();
    CHECK(controllers.size() == 2 * MAX_PLAYERS);
    for (int x = 0; x < MAX_PLAYERS; x++) {
      controller_mapping[controllers[x]] = x;
      controller_mapping[controllers[x + MAX_PLAYERS]] = x;
    }
  }

  for (int team_id = 0; team_id < 2; ++team_id) {
    std::vector<PlayerInfo>& team = team_id == 0
        ? state->left_team : state->right_team;
    team.clear();
    std::vector<Player*> players;
    GetAllTeamPlayers(team_id, players);
    for (auto player : players) {
      auto controller = player->GetExternalController();
      if (controller) {
        if (team_id == 0) {
          state->left_controllers[controller_mapping[
              controller->GetHIDevice()]].controlled_player = team.size();
        } else {
          state->right_controllers[controller_mapping[
              controller->GetHIDevice()]].controlled_player = team.size();
        }
      }
      if (player->CastHumanoid() != NULL) {
        PlayerInfo info;
        info.player_position = player->GetPosition().coords;
        info.player_direction =
            (player->GetMovement() / GetGameConfig().physics_steps_per_frame)
                .coords;
        info.tired_factor = 1 - player->GetFatigueFactorInv();
        info.has_card = player->HasCards();
        info.is_active = player->IsActive();
        info.role = player->GetFormationEntry().role;
        if (player->HasPossession() && GetLastTouchTeamID() != -1 &&
            GetLastTouchTeam()->GetLastTouchPlayer() == player) {
          state->ball_owned_player = team.size();
          state->ball_owned_team = GetLastTouchTeamID();
        }
        team.push_back(info);
      }
    }
  }
}

// THE SPICE

void Match::Process() {
  bool reverse = GetScenarioConfig().reverse_team_processing;
  if (!pause) {

    if (IsInPlay()) {
      MaybeMirror(reverse, !reverse, reverse);
      CheckBallCollisions();
      MaybeMirror(reverse, !reverse, reverse);
    }


    // HIJ IS EEN HONDELUUUL
    MaybeMirror(reverse, !reverse, reverse);
    referee->Process();
    MaybeMirror(reverse, !reverse, reverse);

    // ball

    Vector3 previousBallPos = ball->Predict(0);
    MaybeMirror(false, false, GetScenarioConfig().reverse_team_processing);
    ball->Process();
    MaybeMirror(false, false, GetScenarioConfig().reverse_team_processing);


    // create mental images for the AI to use
    mentalImages.insert(mentalImages.begin(), MentalImage(this));
    if (mentalImages.size() > 30) {
      mentalImages.pop_back();
    }


    // obvious
    teams[first_team]->UpdateSwitch();
    teams[second_team]->UpdateSwitch();

    if (first_team == 0) {
      MaybeMirror(false, true, false);
    } else {
      MaybeMirror(true, false, true);
    }
    teams[first_team]->Process();
    MaybeMirror(true, true, true);
    teams[second_team]->Process();
    if (first_team == 0) {
      MaybeMirror(true, false, true);
    } else {
      MaybeMirror(false, true, false);
    }

    MaybeMirror(reverse, !reverse, reverse);
    officials->Process();
    MaybeMirror(reverse, !reverse, reverse);

    if (first_team == 0) {
      MaybeMirror(false, true, false);
    } else {
      MaybeMirror(true, false, true);
    }
    teams[first_team]->UpdatePossessionStats();
    MaybeMirror(true, true, true);
    teams[second_team]->UpdatePossessionStats();
    if (first_team == 0) {
      MaybeMirror(true, false, true);
    } else {
      MaybeMirror(false, true, false);
    }

    CalculateBestPossessionTeamID();

    if (GetBallRetainer() == 0) {
      if (GetBestPossessionTeam()) {
        Player *candidate = GetBestPossessionTeam()->GetDesignatedTeamPossessionPlayer();
        if (candidate != GetDesignatedPossessionPlayer()) {
          unsigned int designatedTime = GetDesignatedPossessionPlayer()->GetTimeNeededToGetToBall_ms();
          unsigned int candidateTime = candidate->GetTimeNeededToGetToBall_ms();
          float timeRating = (float)(candidateTime + 10) / (float)(designatedTime + 10);
          if (timeRating < 0.85f) designatedPossessionPlayer = candidate;
        }
      } else {
        // just stick with current team
        designatedPossessionPlayer = GetDesignatedPossessionPlayer()->GetTeam()->GetDesignatedTeamPossessionPlayer();
      }
    } else {
      designatedPossessionPlayer = GetBallRetainer();
    }

    bool rev = GetScenarioConfig().reverse_team_processing;
    MaybeMirror(rev, !rev, rev);
    CheckHumanoidCollisions();

    // time
    if (IsInPlay()) {
      matchTime_ms += 10 * (1.0f / matchDurationFactor);
    }
    actualTime_ms += 10;
    if (IsGoalScored()) goalScoredTimer += 10; else goalScoredTimer = 0;

    if (IsInPlay() && !IsInSetPiece()) GetMatchData()->AddPossessionTime_10ms(teams[0] == designatedPossessionPlayer->GetTeam() ? 0 : 1);


    // check for goals

    bool t1goal = CheckForGoal(teams[0]->GetSide(), previousBallPos);
    bool t2goal = CheckForGoal(teams[1]->GetSide(), previousBallPos);
    if (t1goal) ballIsInGoal = true;
    if (t2goal) ballIsInGoal = true;
    MaybeMirror(rev, !rev, rev);
    if (IsInPlay()) {
      if (t1goal) {
        matchData->SetGoalCount(teams[1]->GetID(), matchData->GetGoalCount(1) + 1);
        scoreboard->SetGoalCount(1, matchData->GetGoalCount(1));
        goalScored = true;
        lastGoalTeam = teams[1];
        teams[1]->GetController()->UpdateTactics();
      }
      if (t2goal) {
        matchData->SetGoalCount(teams[0]->GetID(), matchData->GetGoalCount(0) + 1);
        scoreboard->SetGoalCount(0, matchData->GetGoalCount(0));
        goalScored = true;
        lastGoalTeam = teams[0];
        teams[0]->GetController()->UpdateTactics();
      }
      if (t1goal || t2goal) {

        // find out who scored
        bool ownGoal = true;
        if (GetLastTouchTeamID(e_TouchType_Intentional_Kicked) == GetLastGoalTeam()->GetID() || GetLastTouchTeamID(e_TouchType_Intentional_Nonkicked) == GetLastGoalTeam()->GetID()) ownGoal = false;

        if (!ownGoal) {
          lastGoalScorer = GetLastGoalTeam()->GetLastTouchPlayer();
          if (lastGoalScorer) {
            SpamMessage("GOAL for " + GetLastGoalTeam()->GetTeamData()->GetName() + "! " + lastGoalScorer->GetPlayerData()->GetLastName() + " scores!", 4000);
          } else {
            SpamMessage("GOAL!!!", 4000);
          }
        }

        else { // own goal
          lastGoalScorer = teams[abs(GetLastGoalTeam()->GetID() - 1)]->GetLastTouchPlayer();
          if (lastGoalScorer) {
            SpamMessage("OWN GOAL! " + lastGoalScorer->GetPlayerData()->GetLastName() + " is so unlucky!", 4000);
          } else {
            SpamMessage("It's an OWN GOAL! oh noes!", 4000);
          }
        }

      }
    }


    // average possession side

    if (IsInPlay()) {
      if (GetBestPossessionTeam()) {
        float sideValue = 0;
        sideValue += (GetTeam(0)->GetFadingTeamPossessionAmount() - 0.5f) * GetTeam(0)->GetSide();
        sideValue += (GetTeam(1)->GetFadingTeamPossessionAmount() - 0.5f) * GetTeam(1)->GetSide();
        possessionSideHistory.Insert(sideValue);
      }
    }


    if (GetReferee()->GetBuffer().active == true &&
        (GetReferee()->GetCurrentFoulType() == 2 || GetReferee()->GetCurrentFoulType() == 3) &&
        GetReferee()->GetBuffer().stopTime < GetActualTime_ms() - 1000) {

      if (GetReferee()->GetBuffer().prepareTime > GetActualTime_ms()) { // FOUL, film referee
        SetAutoUpdateIngameCamera(false);
        FollowCamera(cameraOrientation, cameraNodeOrientation, cameraNodePosition, cameraFOV, officials->GetReferee()->GetPosition() + Vector3(0, 0, 0.8f), 1.5f);
        cameraNearCap = 1;
        cameraFarCap = 220;
        if (officials->GetReferee()->GetCurrentFunctionType() == e_FunctionType_Special) referee->AlterSetPiecePrepareTime(GetActualTime_ms() + 1000);
      } else { // back to normal
        SetAutoUpdateIngameCamera(true);
      }

    }

  } // end if !pause

  if (autoUpdateIngameCamera) {
    if (GetScenarioConfig().render) {
      MaybeMirror(false, true, false);
      UpdateIngameCamera();
      MaybeMirror(false, true, false);
    }
    if (IsGoalScored() && goalScoredTimer == 6000) {
      pause = true;
    }
  }


  if (!pause) {
    unsigned int zoomTime = 2000;
    unsigned int startTime = 0;
    if (actualTime_ms < zoomTime + startTime) { // nice effect at the start

      Quaternion initialOrientation = QUATERNION_IDENTITY;
      initialOrientation.SetAngleAxis(0.0f * pi, Vector3(1, 0, 0));
      Quaternion zOrientation = QUATERNION_IDENTITY;
      initialOrientation = zOrientation * initialOrientation;

      Vector3 initialPosition = Vector3(0.0f, 0.0f, 60.0);

      int subTime = clamp(actualTime_ms - startTime, 0, zoomTime);
      float bias = subTime / (float)(zoomTime);
      bias *= pi;
      bias = std::sin(bias - 0.5f * pi) * -0.5f + 0.5f;

      cameraOrientation = cameraOrientation.GetSlerped(bias, QUATERNION_IDENTITY);
      cameraNodeOrientation = cameraNodeOrientation.GetSlerped(bias, initialOrientation);
      cameraNodePosition = cameraNodePosition * (1.0f - bias) + initialPosition * bias;
      cameraFOV = cameraFOV * (1.0f - bias) + 40 * bias;
      cameraNearCap = cameraNearCap * (1.0f - bias) + 2.0f * bias;

    }
  } // end if !pause
  iterations++;
}

void Match::PreparePutBuffers() {
  if (!GetPause()) {
    MaybeMirror(false, false, first_team == 1);
    teams[first_team]->PreparePutBuffers();
    MaybeMirror(false, false, true);
    teams[second_team]->PreparePutBuffers();
    MaybeMirror(false, false, first_team == 0);

    MaybeMirror(false, false, first_team == 1);
    officials->PreparePutBuffers();
    MaybeMirror(false, false, first_team == 1);
  }
}

void Match::FetchPutBuffers() {
  if (GetIterations() < 1) return; // no processes done yet
  if (!GetPause()) {
    teams[first_team]->FetchPutBuffers();
    teams[second_team]->FetchPutBuffers();
    officials->FetchPutBuffers();
  }
}

void Match::Put() {
  if (GetIterations() < 2) return; // no processes done yet (todo: this is not the correct way to measure that :p)

  camera->SetPosition(Vector3(0, 0, 0), false);
  camera->SetRotation(cameraOrientation, false);
  cameraNode->SetPosition(cameraNodePosition, false);

  cameraNode->SetRotation(cameraNodeOrientation, false);
  camera->SetFOV(cameraFOV);
  camera->SetCapping(cameraNearCap, cameraFarCap);

  if (!GetPause()) {
    ball->Put();
    teams[first_team]->Put(GetScenarioConfig().symmetric_mode &&
                           GetScenarioConfig().reverse_team_processing);
    teams[second_team]->Put(GetScenarioConfig().symmetric_mode &&
                            !GetScenarioConfig().reverse_team_processing);
    officials->Put();

  }

  GetDynamicNode()->RecursiveUpdateSpatialData(e_SpatialDataType_Both);
  if (!GetScenarioConfig().render) {
    return;
  }
  if (!pause) {
    teams[first_team]->Put2D(GetScenarioConfig().symmetric_mode &&
                             GetScenarioConfig().reverse_team_processing);
    teams[second_team]->Put2D(GetScenarioConfig().symmetric_mode &&
                              !GetScenarioConfig().reverse_team_processing);

    //if (buf_actualTime_ms % 100 == 0) { // a better way would be to count iterations (this modulo is irregular since not all process runs are put)
      // clock

      int seconds = (int)(matchTime_ms / 1000.0) % 60;
      int minutes = (int)(matchTime_ms / 60000.0);

      std::string timeStr = "";
      if (minutes < 10) timeStr += "0";
      timeStr += int_to_str(minutes);
      timeStr += ":";
      if (seconds < 10) timeStr += "0";
      timeStr += int_to_str(seconds);
      scoreboard->SetTimeStr(timeStr);
    //}

    if (messageCaptionRemoveTime_ms <= actualTime_ms) messageCaption->Hide();


    // radar

    radar->Put(GetScenarioConfig().symmetric_mode);


    UpdateGoalNetting(GetBall()->BallTouchesNet());
  } else {
    teams[first_team]->Hide2D();
    teams[second_team]->Hide2D();
  }

}

boost::intrusive_ptr<Node> Match::GetDynamicNode() {
  return dynamicNode;
}

bool Match::CheckForGoal(signed int side, const Vector3& previousBallPos) {
  if (fabs(ball->Predict(10).coords[0]) < pitchHalfW - 1.0) return false;

  Line line;
  line.SetVertex(0, previousBallPos);
  line.SetVertex(1, ball->Predict(0));

  Triangle goal1;
  goal1.SetVertex(0, Vector3((pitchHalfW + lineHalfW + 0.11f) * side, 3.7f, 0));
  goal1.SetVertex(1, Vector3((pitchHalfW + lineHalfW + 0.11f) * side, -3.7f, 0));
  goal1.SetVertex(2, Vector3((pitchHalfW + lineHalfW + 0.11f) * side, 3.7f, 2.5f));
  goal1.SetNormals(Vector3(-side, 0, 0));
  Triangle goal2;
  goal2.SetVertex(0, Vector3((pitchHalfW + lineHalfW + 0.11f) * side, -3.7f, 0));
  goal2.SetVertex(1, Vector3((pitchHalfW + lineHalfW + 0.11f) * side, -3.7f, 2.5f));
  goal2.SetVertex(2, Vector3((pitchHalfW + lineHalfW + 0.11f) * side, 3.7f, 2.5f));
  goal2.SetNormals(Vector3(-side, 0, 0));

  Vector3 intersectVec;
  bool intersect = goal1.IntersectsLine(line, intersectVec);
  if (!intersect) {
    intersect = goal2.IntersectsLine(line, intersectVec);
  }

  // extra check: ball could have gone 'in' via the side netting, if line begin == inside pitch, but outside of post, and line end == in goal. disallow!
  if (fabs(previousBallPos.coords[1]) > 3.7 && fabs(previousBallPos.coords[0]) > pitchHalfW - lineHalfW - 0.11) return false;

  if (intersect) return true; else return false;
}

void Match::CalculateBestPossessionTeamID() {
  if (GetBallRetainer() != 0) {
    bestPossessionTeam = GetBallRetainer()->GetTeam();
  } else {
    int bestTime_ms[2] = { 100000, 100000 };
    bestTime_ms[0] = teams[first_team]->GetTimeNeededToGetToBall_ms();
    bestTime_ms[1] = teams[second_team]->GetTimeNeededToGetToBall_ms();
    if (bestTime_ms[0] < bestTime_ms[1])
      bestPossessionTeam = teams[first_team];
    else if (bestTime_ms[0] > bestTime_ms[1])
      bestPossessionTeam = teams[second_team];
    else {
      assert(bestTime_ms[0] == bestTime_ms[1]);
      bestPossessionTeam = 0;
    }
  }
}

void Match::CheckHumanoidCollisions() {
  std::vector<Player*> players;

  GetTeam(first_team)->GetActivePlayers(players);
  GetTeam(second_team)->GetActivePlayers(players);

  // outer vectors index == players[] index
  std::vector < std::vector<PlayerBounce> > playerBounces;

  // insert an empty entry for every player
  playerBounces.resize(players.size());

  // check each combination of humanoids once
  for (unsigned int i1 = 0; i1 < players.size() - 1; i1++) {
    for (unsigned int i2 = i1 + 1; i2 < players.size(); i2++) {
      CheckHumanoidCollision(players.at(i1), players.at(i2), playerBounces.at(i1), playerBounces.at(i2));
    }
  }

  // do bouncy magic
  for (unsigned int i1 = 0; i1 < players.size(); i1++) {

    float totalForce = 0.0f;

    for (unsigned int i2 = 0; i2 < playerBounces.at(i1).size(); i2++) {

      const PlayerBounce &bounce = playerBounces.at(i1).at(i2);
      totalForce += bounce.force;

    }

    if (totalForce > 0.0f) {

      Vector3 bounceVec;
      for (unsigned int i2 = 0; i2 < playerBounces.at(i1).size(); i2++) {

        const PlayerBounce &bounce = playerBounces.at(i1).at(i2);
        bounceVec += (bounce.opp->GetMovement() - players.at(i1)->GetMovement()) * bounce.force * (bounce.force / totalForce);
      }

      // okay, accumulated all, now distribute them in normalized fashion
      players.at(i1)->OffsetPosition(bounceVec * 0.01f * 1.0f);
    }

  }

}

void Match::CheckHumanoidCollision(Player *p1, Player *p2, std::vector<PlayerBounce> &p1Bounce, std::vector<PlayerBounce> &p2Bounce) {
  constexpr float distanceFactor = 0.72f;
  constexpr float bouncePlayerRadius = 0.5f * distanceFactor;
  constexpr float similarPlayerRadius = 0.8f * distanceFactor;
  constexpr float similarExp = 0.2f;//0.8f;
  constexpr float similarForceFactor = 0.25f; // 0.5f would be the full effect

  Vector3 p1pos = p1->GetPosition();
  Vector3 p2pos = p2->GetPosition();

  float distance = (p1pos - p2pos).GetLength();

  Vector3 p1movement = p1->GetMovement();
  Vector3 p2movement = p2->GetMovement();
  assert(p1movement.coords[2] == 0.0f);
  assert(p2movement.coords[2] == 0.0f);

  float bounceBias = 0.0f;
  Vector3 bounceVec;
  float p1backFacing = 0.5f;
  float p2backFacing = 0.5f;

  if (distance < bouncePlayerRadius * 2.0f ||
      distance < (bouncePlayerRadius + similarPlayerRadius) * 2.0f) {

    bounceVec = (p1pos - p2pos).GetNormalized(Vector3(0, -1, 0));

    // back facing
    Vector3 p1facing = p1->GetDirectionVec().GetRotated2D(p1->GetRelBodyAngle() * 0.7f);
    Vector3 p2facing = p2->GetDirectionVec().GetRotated2D(p2->GetRelBodyAngle() * 0.7f);
    p1backFacing = clamp(p1facing.GetDotProduct( bounceVec) * 0.5f + 0.5f, 0.0f, 1.0f); // 0 .. 1 == worst .. best
    p2backFacing = clamp(p2facing.GetDotProduct(-bounceVec) * 0.5f + 0.5f, 0.0f, 1.0f);

    if (distance < bouncePlayerRadius * 2.0f) {

      bounceBias += p1backFacing * 0.8f;
      bounceBias -= p2backFacing * 0.8f;

      // velocity, faster is worse
      float p1velocity = p1->GetFloatVelocity();
      float p2velocity = p2->GetFloatVelocity();
      bounceBias -= clamp(((p1velocity - p2velocity) / sprintVelocity) * 0.2f, -0.2f, 0.2f);

      if (p1->TouchPending() && p1->GetCurrentFunctionType() == e_FunctionType_Interfere) bounceBias += 0.1f + 0.4f * p1->GetStat(technical_standingtackle);
      if (p1->TouchPending() && p1->GetCurrentFunctionType() == e_FunctionType_Sliding)   bounceBias += 0.1f + 0.4f * p1->GetStat(technical_slidingtackle);
      if (p2->TouchPending() && p2->GetCurrentFunctionType() == e_FunctionType_Interfere) bounceBias -= 0.1f + 0.4f * p2->GetStat(technical_standingtackle);
      if (p2->TouchPending() && p2->GetCurrentFunctionType() == e_FunctionType_Sliding)   bounceBias -= 0.1f + 0.4f * p2->GetStat(technical_slidingtackle);

      // problem is, once possession is lost (usually directly after ball is touched), bias may turn around the other way. (well, maybe that's not a problem. dunno.)
      // if (p1->HasPossession() == true) bounceBias -= 0.3f;
      // if (p2->HasPossession() == true) bounceBias += 0.3f;

      if (p1 == GetDesignatedPossessionPlayer()) bounceBias += 0.4f;
      if (p2 == GetDesignatedPossessionPlayer()) bounceBias -= 0.4f;

      // closest to ball
      if (p1 == p1->GetTeam()->GetDesignatedTeamPossessionPlayer() &&
          p2 == p2->GetTeam()->GetDesignatedTeamPossessionPlayer()) {
        float p1BallDistance = (GetBall()->Predict(10).Get2D() - p1->GetPosition()).GetLength();
        float p2BallDistance = (GetBall()->Predict(10).Get2D() - p2->GetPosition()).GetLength();
        float ballDistanceDiffFactor = clamp(std::min(p2BallDistance, 1.2f) - std::min(p1BallDistance, 1.2f), -0.6f, 0.6f) * 1.0f; // std::min is cap so difference won't matter if ball is far away (so only used in battles about the ball)
        bounceBias += ballDistanceDiffFactor;
      }

      bounceBias += p1->GetStat(physical_balance) * 1.0f;
      bounceBias -= p2->GetStat(physical_balance) * 1.0f;

      bounceBias = clamp(bounceBias, -1.0f, 1.0f);
      bounceBias *= 0.5f;

      // convert bounceBias to 0 .. 1 instead of -1 .. 1
      float bounceBias0to1 = bounceBias * 0.5f + 0.5f;
      //bounceBias0to1 = curve(bounceBias0to1, 0.5f); // more binary

      Vector3 offset1 = (p1pos - p2pos).GetNormalized(0) * (bouncePlayerRadius - distance * 0.5f) * (1.0f - bounceBias0to1) * 2.0f;
      Vector3 offset2 = (p2pos - p1pos).GetNormalized(0) * (bouncePlayerRadius - distance * 0.5f) * bounceBias0to1 * 2.0f;

      // slow down on contact
      /*
      Vector3 averageMomentum = (p1movement + p2movement) * 0.5f;
      offset1 -= averageMomentum * 0.001f;
      offset2 -= averageMomentum * 0.001f;
      */

      // make players snap to the side of opponents (rather, just a bit in front of them too)


      if (GetDesignatedPossessionPlayer() == p2 && p2->HasPossession()) {
        Vector3 p2_leftside = p2pos + p2->GetDirectionVec().GetRotated2D(0.3f * pi) * bouncePlayerRadius * 2;
        Vector3 p2_rightside = p2pos + p2->GetDirectionVec().GetRotated2D(-0.3f * pi) * bouncePlayerRadius * 2;
        float p1_to_p2_left = (p1pos - p2_leftside).GetLength();
        float p1_to_p2_right = (p1pos - p2_rightside).GetLength();
        Vector3 p2side = p1_to_p2_left < p1_to_p2_right ? p2_leftside : p2_rightside;
        // SetYellowDebugPilon(p2side);
        offset1 += (p2side - p1pos).GetNormalizedMax(0.01f) * p1->GetStat(physical_balance) * 0.3f;
      }

      else if (GetDesignatedPossessionPlayer() == p1 && p1->HasPossession()) {
        Vector3 p1_leftside = p1pos + p1->GetDirectionVec().GetRotated2D(0.3f * pi) * bouncePlayerRadius * 2;
        Vector3 p1_rightside = p1pos + p1->GetDirectionVec().GetRotated2D(-0.3f * pi) * bouncePlayerRadius * 2;
        float p2_to_p1_left = (p2pos - p1_leftside).GetLength();
        float p2_to_p1_right = (p2pos - p1_rightside).GetLength();
        Vector3 p1side = p2_to_p1_left < p2_to_p1_right ? p1_leftside : p1_rightside;
        // SetRedDebugPilon(p1side);
        offset2 += (p1side - p2pos).GetNormalizedMax(0.01f) * p2->GetStat(physical_balance) * 0.3f;
      }

      // can not bump faster than sprint
      offset1.NormalizeMax(sprintVelocity * 0.01f);
      offset2.NormalizeMax(sprintVelocity * 0.01f);


      p1->OffsetPosition(offset1);
      p2->OffsetPosition(offset2);
    }


    // take over each others movement a bit (precalc phase)

    float similarBias = 0.0f;

    if (similarForceFactor > 0.0f && distance < (bouncePlayerRadius + similarPlayerRadius) * 2.0f) {
      float shellDistance = std::max(0.0f, distance - bouncePlayerRadius * 2.0f);

      similarBias += p1backFacing * 0.8f;
      similarBias -= p2backFacing * 0.8f;

      // velocity, faster is worse
      float p1velocity = p1->GetFloatVelocity();
      float p2velocity = p2->GetFloatVelocity();
      similarBias -= clamp(((p1velocity - p2velocity) / sprintVelocity) * 0.2f, -0.2f, 0.2f);

      if (p1 == GetDesignatedPossessionPlayer()) similarBias += 0.6f;
      if (p2 == GetDesignatedPossessionPlayer()) similarBias -= 0.6f;

      // closest to ball
      if (p1 == p1->GetTeam()->GetDesignatedTeamPossessionPlayer() &&
          p2 == p2->GetTeam()->GetDesignatedTeamPossessionPlayer()) {
        float p1BallDistance = (GetBall()->Predict(10).Get2D() - p1->GetPosition()).GetLength();
        float p2BallDistance = (GetBall()->Predict(10).Get2D() - p2->GetPosition()).GetLength();
        float ballDistanceDiffFactor = clamp(std::min(p2BallDistance, 1.2f) - std::min(p1BallDistance, 1.2f), -0.6f, 0.6f) * 1.0f; // std::min is cap so difference won't matter if ball is far away (so only used in battles about the ball)
        similarBias += ballDistanceDiffFactor;
      }

      similarBias += p1->GetStat(physical_balance) * 1.0f;
      similarBias -= p2->GetStat(physical_balance) * 1.0f;

      similarBias = clamp(similarBias, -1.0f, 1.0f);
      similarBias *= 0.9f;

      float similarForce = clamp(1.0f - (shellDistance / (similarPlayerRadius * 2.0f)), 0.0f, 1.0f);
      similarForce = std::pow(similarForce, similarExp);
      similarForce *= similarForceFactor;

      assert(similarForce >= 0.0f && similarForce <= 1.0f);

      float similarBias0to1 = similarBias * 0.5f + 0.5f;


      PlayerBounce player1Bounce;
      player1Bounce.opp = p2;
      player1Bounce.force = similarForce * (1.0f - similarBias0to1);
      p1Bounce.push_back(player1Bounce);

      PlayerBounce player2Bounce;
      player2Bounce.opp = p1;
      player2Bounce.force = similarForce * similarBias0to1;
      p2Bounce.push_back(player2Bounce);
    }


    // u b trippin?

    if (distance < bouncePlayerRadius * 2.0f) {

      float p1sensitivity = 0.0f;
      float p2sensitivity = 0.0f;

      p1sensitivity += (1.0f - p1backFacing) * 1.0f;
      p2sensitivity += (1.0f - p2backFacing) * 1.0f;

      // velocity, faster is worse
      float p1velocity = p1->GetFloatVelocity();
      float p2velocity = p2->GetFloatVelocity();
      p1sensitivity += NormalizedClamp(p1velocity, idleVelocity, sprintVelocity) * 1.0f;
      p2sensitivity += NormalizedClamp(p2velocity, idleVelocity, sprintVelocity) * 1.0f;

      if (p1->HasBestPossession() == true) p1sensitivity += 1.0f;
      if (p2->HasBestPossession() == true) p2sensitivity += 1.0f;

      float balanceWeight = 3.0f;
      p1sensitivity += (1.0f - p1->GetStat(physical_balance) * 1.0f) * balanceWeight;
      p2sensitivity += (1.0f - p2->GetStat(physical_balance) * 1.0f) * balanceWeight;

      p1sensitivity += clamp(p1->GetDecayingPositionOffsetLength() * 10.0f, 0.0f, 1.0f);
      p2sensitivity += clamp(p2->GetDecayingPositionOffsetLength() * 10.0f, 0.0f, 1.0f);

      // penetration
      float penetrationWeight = 6.0f;
      float penetration = ( (p1->GetPosition() + p1->GetMovement() * 0.03f) - (p2->GetPosition() + p2->GetMovement() * 0.03f) ).GetLength();
      //if (p1->GetDebug() || p2->GetDebug()) printf("penetration: %f\n", pow(1.0f - NormalizedClamp(penetration, 0.0f, bouncePlayerRadius * 2.0f), 0.4f));
      p1sensitivity +=
          std::pow(1.0f - NormalizedClamp(penetration, 0.0f,
                                          bouncePlayerRadius * 2.0f),
                   0.4f) *
          penetrationWeight;
      p2sensitivity +=
          std::pow(1.0f - NormalizedClamp(penetration, 0.0f,
                                          bouncePlayerRadius * 2.0f),
                   0.4f) *
          penetrationWeight;

      // ball proximity (usually means: stability is less because we sacrifice balance to control the ball)
      float p1BallDistance = (GetBall()->Predict(10).Get2D() - p1->GetPosition()).GetLength();
      float p2BallDistance = (GetBall()->Predict(10).Get2D() - p2->GetPosition()).GetLength();
      p1sensitivity += 1.0f - NormalizedClamp(p1BallDistance, 0.0f, 0.7f);
      p2sensitivity += 1.0f - NormalizedClamp(p2BallDistance, 0.0f, 0.7f);

      // divided by elements active
      p1sensitivity /= 5.0f + balanceWeight + penetrationWeight;
      p2sensitivity /= 5.0f + balanceWeight + penetrationWeight;

      float trip0threshold = 0.38f;
      float trip1threshold = 0.48f;
      float trip2threshold = 0.58f;

      if (p1sensitivity > trip0threshold) {
        int tripType = 0;
        if (p1sensitivity > trip1threshold) tripType = 1;
        if (p1sensitivity > trip2threshold) tripType = 2;
        if (tripType > 0) {
          p1->TripMe((p1->GetMovement() * 0.1f + p2->GetMovement() * 0.06f + bounceVec * 1.0f).GetNormalized(bounceVec), tripType);
          referee->TripNotice(p1, p2, tripType);
        }
      }
      if (p2sensitivity > trip0threshold) {
        int tripType = 0;
        if (p2sensitivity > trip1threshold) tripType = 1;
        if (p2sensitivity > trip2threshold) tripType = 2;
        if (tripType > 0) {
          p2->TripMe((p2->GetMovement() * 0.1f + p1->GetMovement() * 0.06f - bounceVec * 1.0f).GetNormalized(-bounceVec), tripType);
          referee->TripNotice(p2, p1, tripType);
        }
      }

    } // within either bump, similar or trip range

  }


  // check for tackling collisions

  int tackle = 0;
  if ((p1->GetCurrentFunctionType() == e_FunctionType_Sliding || p1->GetCurrentFunctionType() == e_FunctionType_Interfere) && p1->GetFrameNum() > 5 && p1->GetFrameNum() < 28) tackle += 1;
  if ((p2->GetCurrentFunctionType() == e_FunctionType_Sliding || p2->GetCurrentFunctionType() == e_FunctionType_Interfere) && p2->GetFrameNum() > 5 && p2->GetFrameNum() < 28) tackle += 2;
  if (distance < 2.0f && tackle > 0 && tackle < 3) { // if tackle is 3, ignore both
    std::list < boost::intrusive_ptr<Geometry> > tacklerObjectList;
    std::list < boost::intrusive_ptr<Geometry> > victimObjectList;
    /*
    if (tackle == 0) {
      if (p1->GetCurrentFunctionType() == e_FunctionType_Trap ||
          p1->GetCurrentFunctionType() == e_FunctionType_ShortPass ||
          p1->GetCurrentFunctionType() == e_FunctionType_LongPass ||
          p1->GetCurrentFunctionType() == e_FunctionType_HighPass ||
          p1->GetCurrentFunctionType() == e_FunctionType_Shot ||
          p1->GetCurrentFunctionType() == e_FunctionType_Interfere) {
        p1->GetHumanoidNode()->GetObjects(e_ObjectType_Geometry, tacklerObjectList);
        p2->GetHumanoidNode()->GetObjects(e_ObjectType_Geometry, victimObjectList);
        p1action = true;
      }
      else if (p2->GetCurrentFunctionType() == e_FunctionType_Trap ||
               p2->GetCurrentFunctionType() == e_FunctionType_ShortPass ||
               p2->GetCurrentFunctionType() == e_FunctionType_LongPass ||
               p2->GetCurrentFunctionType() == e_FunctionType_HighPass ||
               p2->GetCurrentFunctionType() == e_FunctionType_Shot ||
               p2->GetCurrentFunctionType() == e_FunctionType_Interfere) {
        p2->GetHumanoidNode()->GetObjects(e_ObjectType_Geometry, tacklerObjectList);
        p1->GetHumanoidNode()->GetObjects(e_ObjectType_Geometry, victimObjectList);
        p2action = true;
      }
    }
    */
    if (tackle == 1) {
      p1->GetHumanoidNode()->GetObjects(e_ObjectType_Geometry, tacklerObjectList);
      p2->GetHumanoidNode()->GetObjects(e_ObjectType_Geometry, victimObjectList);
    }
    if (tackle == 2) {
      p2->GetHumanoidNode()->GetObjects(e_ObjectType_Geometry, tacklerObjectList);
      p1->GetHumanoidNode()->GetObjects(e_ObjectType_Geometry, victimObjectList);
    }

    // iterate through all body parts of tackler
    std::list < boost::intrusive_ptr<Geometry> >::iterator objIter = tacklerObjectList.begin();
    while (objIter != tacklerObjectList.end()) {

      AABB objAABB = (*objIter)->GetAABB();

      // make a tad smaller: AABBs are usually too large.
      objAABB.minxyz += 0.1f;
      objAABB.maxxyz -= 0.1f;

      std::list < boost::intrusive_ptr<Geometry> >::iterator victimIter = victimObjectList.begin();
      while (victimIter != victimObjectList.end()) {

        std::string bodyPartName = (*victimIter)->GetName();
        if (bodyPartName.compare("left_foot") == 0 || bodyPartName.compare("right_foot") == 0 ||
            bodyPartName.compare("left_lowerleg") == 0 || bodyPartName.compare("right_lowerleg") == 0
            /*bodyPartName == "left_upperleg" || bodyPartName == "right_upperleg"*/) {
          if (objAABB.Intersects((*victimIter)->GetAABB())) {
            //printf("HIT: %s hits %s\n", (*objIter)->GetName().c_str(), (*victimIter)->GetName().c_str());

            if (tackle == 1) {
              if (p1->GetFrameNum() > 10 && p1->GetFrameNum() < p1->GetFrameCount() - 6) {
                Vector3 tripVec = p2->GetDirectionVec();
                int tripType = 3; // sliding
                if (p1->GetCurrentFunctionType() == e_FunctionType_Interfere) tripType = 1; // was 2
                p2->TripMe(tripVec, tripType);
                referee->TripNotice(p2, p1, tripType);
              }
            }
            if (tackle == 2) {
              if (p2->GetFrameNum() > 10 && p2->GetFrameNum() < p2->GetFrameCount() - 6) {
                Vector3 tripVec = p1->GetDirectionVec();
                int tripType = 3; // sliding
                if (p2->GetCurrentFunctionType() == e_FunctionType_Interfere) tripType = 1; // was 2
                p1->TripMe(tripVec, tripType);
                referee->TripNotice(p1, p2, tripType);
              }
            }
            break;
          }
        }

        victimIter++;
      }

      objIter++;
    }
  }

}

void Match::CheckBallCollisions() {



  //printf("%i - %i hihi\n", actualTime_ms, lastBodyBallCollisionTime_ms + 150);
  if (actualTime_ms <= lastBodyBallCollisionTime_ms + 150) return;

  std::vector<Player*> players;
  GetTeam(first_team)->GetActivePlayers(players);
  GetTeam(second_team)->GetActivePlayers(players);

  std::list < boost::intrusive_ptr<Geometry> > objectList;
  Vector3 bounceVec;
  float bias = 0.0;
  int bounceCount = 0; // this shit is shit, average properly in combination with bias or something like that

  //printf("lasttouchbias: %f, isnul?: %s\n", GetLastTouchBias(200), GetLastTouchBias(200) == 0.0f ? "true" : "false");
  for (int i = 0; i < (signed int)players.size(); i++) {

    bool biggestRatio = false;
    int teamID = players[i]->GetTeam()->GetID();

    int touchTimeThreshold_ms = 200;//700;
    float oppLastTouchBias = GetTeam(abs(teamID - 1))->GetLastTouchBias(touchTimeThreshold_ms);
    float lastTouchBias = players[i]->GetLastTouchBias(touchTimeThreshold_ms);
    float oppLastTouchBiasLong = GetTeam(abs(teamID - 1))->GetLastTouchBias(1600);

    if (lastTouchBias <= 0.01f && oppLastTouchBias > 0.01f/* && ballTowardsPlayer*/) { // cannot collide if opp didn't recently touch ball (we would be able to predict ball by then), or if player itself already did (to overcome the 'perpetuum collision' problem, and to allow for 'controlled ball collisions' in humanoid class)

      bool collisionAnim = false;
      if (players[i]->GetCurrentFunctionType() == e_FunctionType_Movement || players[i]->GetCurrentFunctionType() == e_FunctionType_Trip || players[i]->GetCurrentFunctionType() == e_FunctionType_Sliding || players[i]->GetCurrentFunctionType() == e_FunctionType_Interfere || players[i]->GetCurrentFunctionType() == e_FunctionType_Deflect) collisionAnim = true;
      bool onlyWhenDirectionChangedUnexpectedly = false;
      if (players[i]->GetCurrentFunctionType() == e_FunctionType_Interfere || players[i]->GetCurrentFunctionType() == e_FunctionType_Deflect) onlyWhenDirectionChangedUnexpectedly = true;

      bool directionChangedUnexpectedly = false;
      if (onlyWhenDirectionChangedUnexpectedly) {
        float unexpectedDistance = (GetMentalImage(players[i]->GetController()->GetReactionTime_ms() + players[i]->GetFrameNum() * 10)->GetBallPrediction(1000) - GetBall()->Predict(1000)).GetLength(); // mental image from when the anim began
        if (unexpectedDistance > 0.5f) directionChangedUnexpectedly = true;
      }


      if (collisionAnim && !players[i]->HasUniquePossession() && (onlyWhenDirectionChangedUnexpectedly == directionChangedUnexpectedly)) {

        float boundingBoxSizeOffset = -0.1f; // fake a big AABB for more blocking fun, or a small one for less bouncy bounce
        if (!players[i]->HasPossession()) boundingBoxSizeOffset += 0.03f; else
                                          boundingBoxSizeOffset -= 0.03f;

        if (players[i]->GetCurrentFunctionType() == e_FunctionType_Sliding || players[i]->GetCurrentFunctionType() == e_FunctionType_Interfere) {
          boundingBoxSizeOffset += 0.1f;
        }
        if (players[i]->GetCurrentFunctionType() == e_FunctionType_Deflect) {
          boundingBoxSizeOffset += 0.2f;
        }

        if (((players[i]->GetPosition() + Vector3(0, 0, 0.8f)) - ball->Predict(0)).GetLength() < 2.5f) { // premature optimization is the root of all evil :D
          players[i]->GetHumanoidNode()->GetObjects(e_ObjectType_Geometry, objectList);

          std::list < boost::intrusive_ptr<Geometry> >::iterator objIter = objectList.begin();
          while (objIter != objectList.end()) {

            AABB objAABB = (*objIter)->GetAABB();
            float ballRadius = 0.11f + boundingBoxSizeOffset;
            if (objAABB.Intersects(ball->Predict(0), ballRadius)) {
              if (players[i] == players[i]->GetTeam()->GetDesignatedTeamPossessionPlayer() && GetLastTouchBias(200) < 0.01f) {

                players[i]->TriggerControlledBallCollision();

              } else {


                float movementBias = oppLastTouchBias * 0.8f + 0.2f;
                bounceVec += (ball->Predict(0) - (*objIter)->GetDerivedPosition()).GetNormalized(Vector3(0)) * movementBias + players[i]->GetMovement() * (1.0f - movementBias);
                bounceCount++;
                players[i]->GetTeam()->SetLastTouchPlayer(players[i], e_TouchType_Accidental);
                Vector3 aabbCenter;
                objAABB.GetCenter(aabbCenter);
                bias += (1.0f - clamp(((ball->Predict(0) - aabbCenter).GetLength() - ballRadius) / objAABB.GetRadius(), 0.0f, 1.0f)) * 0.9f + 0.1f;

              }

            }

            objIter++;
          }

        }

      }
    }
  }

  if (bias > 0.0f) {
    bounceVec /= (bounceCount * 1.0f);
    bounceVec.coords[2] *= 0.6f;
    bounceVec.Normalize();
    Vector3 currentMovement = ball->GetMovement();
    Vector3 fullCollisionVec = (bounceVec * 6.0f) + (bounceVec * currentMovement.GetLength() * 0.6f) + (currentMovement * -0.2f);
    bias = clamp(bias, 0.0f, 1.0f);
    bias = bias * 0.5f + 0.5f;
    Vector3 resultVector = fullCollisionVec * bias + currentMovement * (1.0f - bias);
    if (resultVector.GetLength() > currentMovement.GetLength()) resultVector = resultVector.GetNormalized(0) * currentMovement.GetLength();
    //resultVector = resultVector.GetNormalized(0) * (currentMovement.GetLength() * 0.7f + resultVector.GetLength() * 0.3f); // EXPERIMENT!
    resultVector *= 0.7f;

    ball->Touch(resultVector);
    ball->SetRotation(boostrandom(-30, 30), boostrandom(-30, 30),
                      boostrandom(-30, 30), 0.5f * bias);
    lastBodyBallCollisionTime_ms = actualTime_ms;
  }

}

void Match::FollowCamera(Quaternion &orientation, Quaternion &nodeOrientation, Vector3 &position, float &FOV, const Vector3 &targetPosition, float zoom) {
  orientation.SetAngleAxis(0.4f * pi, Vector3(1, 0, 0));
  nodeOrientation.SetAngleAxis(targetPosition.GetAngle2D() + 1.5 * pi, Vector3(0, 0, 1));
  position = targetPosition - targetPosition.Get2D().GetNormalized(Vector3(0, -1, 0)) * 10 * (1.0f / zoom) + Vector3(0, 0, 3);
  FOV = 60.0f;
}

int Match::GetReplaySize_ms() {
  return replaySize_ms;
}

void Match::PrepareGoalNetting() {

  // collect vertices into nettingMeshes[0..1]
  std::vector < MaterializedTriangleMesh > &triangleMesh = boost::static_pointer_cast<Geometry>(goalsNode->GetObject("goals"))->GetGeometryData()->GetResource()->GetTriangleMeshesRef();

  for (unsigned int m = 0; m < triangleMesh.size(); m++) {
    for (int i = 0; i < triangleMesh.at(m).verticesDataSize / GetTriangleMeshElementCount(); i+= 3) {
      int goalID = -1;
      if (triangleMesh.at(m).vertices[i + 0] < -pitchHalfW - 0.06f) goalID = 0; // don't catch woodwork, only netting.. DIRTY HAXX
      if (triangleMesh.at(m).vertices[i + 0] >  pitchHalfW + 0.06f) goalID = 1;
      if (goalID >= 0) {
        nettingMeshesSrc[goalID].push_back(Vector3(triangleMesh.at(m).vertices[i + 0], triangleMesh.at(m).vertices[i + 1], triangleMesh.at(m).vertices[i + 2]));
        nettingMeshes[goalID].push_back(&(triangleMesh.at(m).vertices[i]));
      }
    }
  }

}

void Match::UpdateGoalNetting(bool ballTouchesNet) {

  nettingHasChanged = false;
  int sideID = (ball->GetBallGeom()->GetPosition().coords[0] < 0) ? 0 : 1;
  if (ballTouchesNet) {
    // find vertex closest to ball
    float shortestDistance = 100000.0f;
    //int shortestDistanceID = 0;
    for (unsigned int i = 0; i < nettingMeshes[sideID].size(); i++) {
      Vector3 vertex = nettingMeshesSrc[sideID][i];
      float distance = vertex.GetDistance(ball->GetBallGeom()->GetPosition());
      if (distance < shortestDistance) {
        shortestDistance = distance;
        //shortestDistanceID = i;
      }
    }

    // pull vertices towards ball - the closer, the more intense
    for (unsigned int i = 0; i < nettingMeshes[sideID].size(); i++) {
      const Vector3 &vertex = nettingMeshesSrc[sideID][i];
      float falloffDistance = 4.0f;
      //float influenceBias = clamp(1.0f - (vertex.GetDistance(ball->GetBallGeom()->GetPosition()) - shortestDistance) / falloffDistance, 0.0f, 1.0f);
      float influenceBias = std::pow(
          clamp((shortestDistance + 0.0001f) /
                    (vertex.GetDistance(ball->GetBallGeom()->GetPosition()) +
                     0.0001f),
                0.0f, 1.0f),
          1.5f);
      // net is stuck to woodwork so lay off there
      float woodworkTensionBiasInv = clamp((fabs(ball->GetBallGeom()->GetPosition().coords[0]) - pitchHalfW) * 2.0f, 0.0f, 1.0f);
      influenceBias *= woodworkTensionBiasInv;
      // http://www.wolframalpha.com/input/?i=sin%28x+*+pi+-+0.5+*+pi%29+*+0.5+%2B+0.5+from+x+%3D+0+to+1
      influenceBias = std::sin(influenceBias * pi - 0.5f * pi) * 0.5f + 0.5f;
      if (influenceBias > 0.0f) {
        Vector3 result = vertex * (1.0f - influenceBias) + ball->GetBallGeom()->GetPosition() * influenceBias;
        static_cast<float*>(nettingMeshes[sideID][i])[0] = result.coords[0];
        static_cast<float*>(nettingMeshes[sideID][i])[1] = result.coords[1];
        static_cast<float*>(nettingMeshes[sideID][i])[2] = result.coords[2];
      }
    }
    resetNetting = true; // make sure to reset next time
    nettingHasChanged = true;

  } else if (resetNetting) { // ball doesn't touch net (anymore), reset
    for (int sideID = 0; sideID < 2; sideID++) {
      for (unsigned int i = 0; i < nettingMeshes[sideID].size(); i++) {
        static_cast<float*>(nettingMeshes[sideID][i])[0] = nettingMeshesSrc[sideID][i].coords[0];
        static_cast<float*>(nettingMeshes[sideID][i])[1] = nettingMeshesSrc[sideID][i].coords[1];
        static_cast<float*>(nettingMeshes[sideID][i])[2] = nettingMeshesSrc[sideID][i].coords[2];
      }
    }
    resetNetting = false;
    nettingHasChanged = true;
  }

}

void Match::UploadGoalNetting() {
  if (nettingHasChanged) {
    boost::static_pointer_cast<Geometry>(goalsNode->GetObject("goals"))->OnUpdateGeometryData(false);
  }
}
