#include "bot/bot.hpp"
#include "linux-workaround/early-input.hpp"
#include "physics/collisions.hpp"
#include "physics/gjbasegamelayer.hpp"
#include "physics/player.hpp"
#include "timestamp.hpp"
#include "trajectory/trajectory.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/DashRingObject.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJGroundLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>

using namespace geode::prelude;

static bool g_softToggle = false;
static bool g_extrapolating = false;
static bool g_leadLog = false;

enum class RenderLead { None, Adaptive, Constant, CpuSmooth, Cpu };
static RenderLead g_renderLead = RenderLead::Constant;

static RenderLead renderLeadFromString(std::string const &value) {
  if (value == "none") {
    return RenderLead::None;
  }
  if (value == "constant") {
    return RenderLead::Constant;
  }
  if (value == "cpu-smooth") {
    return RenderLead::CpuSmooth;
  }
  if (value == "cpu") {
    return RenderLead::Cpu;
  }
  return RenderLead::Constant;
}

// for logging
static const char *renderLeadName(RenderLead mode) {
  switch (mode) {
  case RenderLead::None:
    return "none";
  case RenderLead::Adaptive:
    return "adaptive";
  case RenderLead::Constant:
    return "constant";
  case RenderLead::CpuSmooth:
    return "cpu-smooth";
  case RenderLead::Cpu:
    return "cpu";
  }
  return "?";
}

$on_mod(Loaded) {
  g_softToggle = Mod::get()->getSettingValue<bool>("soft-toggle");
  listenForSettingChanges<bool>("soft-toggle",
                                [](bool value) { g_softToggle = value; });

  g_renderLead = renderLeadFromString(
      Mod::get()->getSettingValue<std::string>("render-lead"));
  listenForSettingChanges<std::string>("render-lead", [](std::string value) {
    g_renderLead = renderLeadFromString(value);
  });

  g_leadLog = Mod::get()->getSettingValue<bool>("lead-log");
  listenForSettingChanges<bool>("lead-log",
                                [](bool value) { g_leadLog = value; });

  earlyInputSetup();
}

static void extrapolatePushButton(PlayerObject *player, PlayerButton button) {
  player->pushButton(button);
}

static void extrapolateReleaseButton(PlayerObject *player,
                                     PlayerButton button) {
  player->releaseButton(button);
}

struct PlayerState {
  CCPoint lastPos = {0, 0};
  CCPoint lastVel = {0, 0};
  CCPoint prevVel = {0, 0};
  float lastRot = 0;
  double lastTime = 0;
  double prevTime = 0;
  float lastDt = 0;
  int lastSteps = 0;
  int steps = 0;
  double prog = 0;
  double tickTime = 0;
  bool isDead = false;
};

static void syncFakePlayer(PlayerObject *fake, PlayerObject *real) {
  if (!fake || !real)
    return;
  fake->copyAttributes(real);

  fake->setPosition(real->getPosition());
  fake->setRotation(real->getRotation());
  fake->m_position = real->m_position;
  fake->m_positionX = real->m_positionX;
  fake->m_positionY = real->m_positionY;
  fake->m_unmodifiedPositionX = real->m_unmodifiedPositionX;
  fake->m_unmodifiedPositionY = real->m_unmodifiedPositionY;
  fake->m_lastPosition = real->m_lastPosition;
  fake->m_lastPortalPos = real->m_lastPortalPos;

  fake->m_yVelocity = real->m_yVelocity;
  fake->m_platformerXVelocity = real->m_platformerXVelocity;
  fake->m_xVelocityRelated = real->m_xVelocityRelated;
  fake->m_xVelocityRelated2 = real->m_xVelocityRelated2;
  fake->m_gravity = real->m_gravity;
  fake->m_gravityMod = real->m_gravityMod;
  fake->m_speedMultiplier = real->m_speedMultiplier;
  fake->m_playerSpeed = real->m_playerSpeed;
  fake->m_vehicleSize = real->m_vehicleSize;
  fake->m_isUpsideDown = real->m_isUpsideDown;
  fake->setFlipY(real->isFlipY());
  fake->setFlipX(real->isFlipX());

  fake->m_isOnGround = real->m_isOnGround;
  fake->m_isOnGround2 = real->m_isOnGround2;
  fake->m_isOnGround3 = real->m_isOnGround3;
  fake->m_isOnGround4 = real->m_isOnGround4;
  fake->m_isOnSlope = real->m_isOnSlope;
  fake->m_wasOnSlope = real->m_wasOnSlope;
  fake->m_slopeRotation = real->m_slopeRotation;
  fake->m_slopeAngle = real->m_slopeAngle;
  fake->m_slopeAngleRadians = real->m_slopeAngleRadians;
  fake->m_currentSlope = real->m_currentSlope;
  fake->m_currentPotentialSlope = real->m_currentPotentialSlope;
  fake->m_lastGroundObject = real->m_lastGroundObject;
  fake->m_preLastGroundObject = real->m_preLastGroundObject;
  fake->m_maybeLastGroundObject = real->m_maybeLastGroundObject;
  fake->m_collidingWithSlopeId = real->m_collidingWithSlopeId;
  fake->m_slopeFlipGravityRelated = real->m_slopeFlipGravityRelated;
  fake->m_potentialSlopeMap = real->m_potentialSlopeMap;

  fake->m_collidedObject = real->m_collidedObject;
  fake->m_collidingWithLeft = real->m_collidingWithLeft;
  fake->m_collidingWithRight = real->m_collidingWithRight;
  fake->m_isCollidingWithSlope = real->m_isCollidingWithSlope;
  fake->m_maybeIsColliding = real->m_maybeIsColliding;
  fake->m_maybeTouchedBreakableBlock = real->m_maybeTouchedBreakableBlock;
  fake->m_touchedPad = real->m_touchedPad;
  fake->m_isGoingLeft = real->m_isGoingLeft;
  fake->m_isSideways = real->m_isSideways;

  fake->m_lastCollisionBottom = real->m_lastCollisionBottom;
  fake->m_lastCollisionTop = real->m_lastCollisionTop;
  fake->m_lastCollisionLeft = real->m_lastCollisionLeft;
  fake->m_lastCollisionRight = real->m_lastCollisionRight;
  fake->m_collidedTopMinY = real->m_collidedTopMinY;
  fake->m_collidedBottomMaxY = real->m_collidedBottomMaxY;
  fake->m_collidedLeftMaxX = real->m_collidedLeftMaxX;
  fake->m_collidedRightMinX = real->m_collidedRightMinX;

  fake->m_touchedRings = real->m_touchedRings;
  if (fake->m_touchingRings && real->m_touchingRings) {
    fake->m_touchingRings->removeAllObjects();
    for (unsigned int i = 0; i < real->m_touchingRings->count(); i++) {
      fake->m_touchingRings->addObject(real->m_touchingRings->objectAtIndex(i));
    }
  }
  fake->m_touchedRing = real->m_touchedRing;
  fake->m_touchedCustomRing = real->m_touchedCustomRing;
  fake->m_touchedGravityPortal = real->m_touchedGravityPortal;
  fake->m_ringRelatedSet = real->m_ringRelatedSet;
  fake->m_lastActivatedPortal = real->m_lastActivatedPortal;

  if (fake->m_collisionLogTop)
    fake->m_collisionLogTop->removeAllObjects();
  if (fake->m_collisionLogBottom)
    fake->m_collisionLogBottom->removeAllObjects();
  if (fake->m_collisionLogLeft)
    fake->m_collisionLogLeft->removeAllObjects();
  if (fake->m_collisionLogRight)
    fake->m_collisionLogRight->removeAllObjects();

  fake->m_holdingLeft = real->m_holdingLeft;
  fake->m_holdingRight = real->m_holdingRight;
  fake->m_holdingButtons = real->m_holdingButtons;
  fake->m_jumpBuffered = real->m_jumpBuffered;
  fake->m_wasJumpBuffered = real->m_wasJumpBuffered;
  fake->m_hasEverJumped = real->m_hasEverJumped;
  fake->m_isDashing = real->m_isDashing;
  fake->m_isDead = real->m_isDead;
  fake->m_inputsLocked = real->m_inputsLocked;
  fake->m_totalTime = real->m_totalTime;

  fake->m_isShip = real->m_isShip;
  fake->m_isBird = real->m_isBird;
  fake->m_isBall = real->m_isBall;
  fake->m_isDart = real->m_isDart;
  fake->m_isRobot = real->m_isRobot;
  fake->m_isSpider = real->m_isSpider;
  fake->m_isSwing = real->m_isSwing;
  fake->m_playEffects = false;

  fake->m_stateHitHead = real->m_stateHitHead;
  fake->m_stateDartSlide = real->m_stateDartSlide;
  fake->m_stateNoAutoJump = real->m_stateNoAutoJump;
  fake->m_stateFlipGravity = real->m_stateFlipGravity;
  fake->m_stateForce = real->m_stateForce;
  fake->m_stateForceVector = real->m_stateForceVector;
  fake->m_jumpPadRelated = real->m_jumpPadRelated;

  fake->m_dashX = real->m_dashX;
  fake->m_dashY = real->m_dashY;
  fake->m_dashAngle = real->m_dashAngle;
  fake->m_dashStartTime = real->m_dashStartTime;
  fake->m_dashRing = real->m_dashRing;

  if (fake->m_waveTrail && fake->m_waveTrail->m_pointArray) {
    fake->m_waveTrail->m_pointArray->removeAllObjects();
  }
  if (fake->m_regularTrail) {
    fake->m_regularTrail->stopStroke();
  }
  if (fake->m_shipStreak) {
    fake->m_shipStreak->stopStroke();
  }
}

static bool isFakePlayer(PlayerObject *player);

static void cleanUpFakePlayer(PlayerObject *&player) {
  if (!player)
    return;

  if (Bot::get()->trajectory().m_fakePlayer1 == player) {
    Bot::get()->trajectory().m_fakePlayer1 = nullptr;
  }
  if (Bot::get()->trajectory().unsafeInner()->m_fakePlayer1 == player) {
    Bot::get()->trajectory().unsafeInner()->m_fakePlayer1 = nullptr;
  }
  if (Bot::get()->trajectory().m_fakePlayer2 == player) {
    Bot::get()->trajectory().m_fakePlayer2 = nullptr;
  }
  if (Bot::get()->trajectory().unsafeInner()->m_fakePlayer2 == player) {
    Bot::get()->trajectory().unsafeInner()->m_fakePlayer2 = nullptr;
  }

  player->release();
  player = nullptr;
}

class $modify(MyBGL, GJBaseGameLayer) {
  struct CameraState {
    float cameraFlip;
    float cameraWidthOffset;
    float cameraHeightOffset;
    float cameraUnzoomedHeightOffset;
    float targetCameraHeightOffset;
    bool calculateTargetHeightOffset;
    bool staticCameraShake;
    bool skipCameraShake;
    float cameraWidth;
    float cameraHeight;
    float cameraUnzoomedX;
    float halfCameraWidth;
    float unk31f8;
    bool unk322a;
    cocos2d::CCPoint cameraPosition;
    cocos2d::CCPoint cameraOffset;
    float cameraZoom;
    float cameraAngle;
  };

  CameraState saveCameraState() {
    CameraState state;
    state.cameraFlip = m_cameraFlip;
    state.cameraWidthOffset = m_cameraWidthOffset;
    state.cameraHeightOffset = m_cameraHeightOffset;
    state.cameraUnzoomedHeightOffset = m_cameraUnzoomedHeightOffset;
    state.targetCameraHeightOffset = m_targetCameraHeightOffset;
    state.calculateTargetHeightOffset = m_calculateTargetHeightOffset;
    state.staticCameraShake = m_staticCameraShake;
    state.skipCameraShake = m_skipCameraShake;
    state.cameraWidth = m_cameraWidth;
    state.cameraHeight = m_cameraHeight;
    state.cameraUnzoomedX = m_cameraUnzoomedX;
    state.halfCameraWidth = m_halfCameraWidth;
    state.unk31f8 = m_unk31f8;
    state.unk322a = m_unk322a;
    state.cameraPosition = m_gameState.m_cameraPosition;
    state.cameraOffset = m_gameState.m_cameraOffset;
    state.cameraZoom = m_gameState.m_cameraZoom;
    state.cameraAngle = m_gameState.m_cameraAngle;
    return state;
  }

  void restoreCameraState(const CameraState &state) {
    m_cameraFlip = state.cameraFlip;
    m_cameraWidthOffset = state.cameraWidthOffset;
    m_cameraHeightOffset = state.cameraHeightOffset;
    m_cameraUnzoomedHeightOffset = state.cameraUnzoomedHeightOffset;
    m_targetCameraHeightOffset = state.targetCameraHeightOffset;
    m_calculateTargetHeightOffset = state.calculateTargetHeightOffset;
    m_staticCameraShake = state.staticCameraShake;
    m_skipCameraShake = state.skipCameraShake;
    m_cameraWidth = state.cameraWidth;
    m_cameraHeight = state.cameraHeight;
    m_cameraUnzoomedX = state.cameraUnzoomedX;
    m_halfCameraWidth = state.halfCameraWidth;
    m_unk31f8 = state.unk31f8;
    m_unk322a = state.unk322a;
    m_gameState.m_cameraPosition = state.cameraPosition;
    m_gameState.m_cameraOffset = state.cameraOffset;
    m_gameState.m_cameraZoom = state.cameraZoom;
    m_gameState.m_cameraAngle = state.cameraAngle;
  }

  struct GroundState {
    float x;
    float y;
    float scaleX;
    float scaleY;
    float rotation;
    float offset;
    float unk;
    bool showGround;
    bool showGround1;
    bool showGround2;
    bool cameraRotated;
  };

  GroundState saveGroundState(GJGroundLayer *ground) {
    GroundState state = {0};
    if (ground) {
      state.x = ground->getPositionX();
      state.y = ground->getPositionY();
      state.scaleX = ground->getScaleX();
      state.scaleY = ground->getScaleY();
      state.rotation = ground->getRotation();
      state.offset = ground->m_ground1Offset;
      state.unk = ground->m_unk1cc;
      state.showGround = ground->m_showGround;
      state.showGround1 = ground->m_showGround1;
      state.showGround2 = ground->m_showGround2;
      state.cameraRotated = ground->m_cameraRotated;
    }
    return state;
  }

  void restoreGroundState(GJGroundLayer *ground, const GroundState &state) {
    if (ground) {
      ground->setPositionX(state.x);
      ground->setPositionY(state.y);
      ground->setScaleX(state.scaleX);
      ground->setScaleY(state.scaleY);
      ground->setRotation(state.rotation);
      ground->m_ground1Offset = state.offset;
      ground->m_unk1cc = state.unk;
      ground->m_showGround = state.showGround;
      ground->m_showGround1 = state.showGround1;
      ground->m_showGround2 = state.showGround2;
      ground->m_cameraRotated = state.cameraRotated;
    }
  }

  struct SavedNodeState {
    cocos2d::CCNode *node;
    cocos2d::CCPoint position;
    float rotation;
    float scaleX;
    float scaleY;
    bool visible;
    unsigned char opacity;
    bool hasOpacity;
  };

  void saveNodePositionsRecursive(cocos2d::CCNode *node,
                                  std::vector<SavedNodeState> &saved) {
    if (!node)
      return;
    unsigned char opacity = 255;
    bool hasOpacity = false;
    if (auto rgba = dynamic_cast<cocos2d::CCRGBAProtocol *>(node)) {
      opacity = rgba->getOpacity();
      hasOpacity = true;
    }
    saved.push_back({node, node->getPosition(), node->getRotation(),
                     node->getScaleX(), node->getScaleY(), node->isVisible(),
                     opacity, hasOpacity});
    if (node->getChildren()) {
      for (auto *child :
           geode::cocos::CCArrayExt<cocos2d::CCNode *>(node->getChildren())) {
        saveNodePositionsRecursive(child, saved);
      }
    }
  }

  void
  collectAliveNodesRecursive(cocos2d::CCNode *node,
                             std::unordered_set<cocos2d::CCNode *> &alive) {
    if (!node)
      return;
    alive.insert(node);
    if (node->getChildren()) {
      for (auto *child :
           geode::cocos::CCArrayExt<cocos2d::CCNode *>(node->getChildren())) {
        collectAliveNodesRecursive(child, alive);
      }
    }
  }

  void restoreNodePositions(const std::vector<SavedNodeState> &saved,
                            cocos2d::CCNode *root) {
    if (!root)
      return;
    std::unordered_set<cocos2d::CCNode *> alive;
    collectAliveNodesRecursive(root, alive);

    for (const auto &state : saved) {
      if (alive.contains(state.node)) {
        state.node->setPosition(state.position);
        state.node->setRotation(state.rotation);
        state.node->setScaleX(state.scaleX);
        state.node->setScaleY(state.scaleY);
        state.node->setVisible(state.visible);
        if (state.hasOpacity) {
          if (auto rgba = dynamic_cast<cocos2d::CCRGBAProtocol *>(state.node)) {
            rgba->setOpacity(state.opacity);
          }
        }
      }
    }
  }

  struct Fields {
    PlayerState p1;
    PlayerState p2;
    PlayerObject *m_fakePlayer1 = nullptr;
    PlayerObject *m_fakePlayer2 = nullptr;
    bool m_enableSolidCollisions = true;
    double m_teleportYOffset = 0.0;

    double m_lastVisitTime = 0.0;

    // adaptive recent worst negative remainder
    double m_minRemainder = 0.0;
    bool m_minRemainderInit = false;

    // cpu-smooth low-passed clock lag
    double m_lagSlow = 0.0;
    bool m_lagSlowInit = false;

    // lead log
    double m_leadWindowStart = 0.0;
    double m_leadSum = 0.0;
    double m_leadMin = 0.0;
    double m_leadMax = 0.0;
    double m_leadDtSum = 0.0;
    int m_leadCount = 0;

    ~Fields() {
      cleanUpFakePlayer(m_fakePlayer1);
      cleanUpFakePlayer(m_fakePlayer2);
    }
  };

  static void onModify(auto &self) {
    (void)self.setHookPriority("GJBaseGameLayer::update", Priority::VeryEarly);
    (void)self.setHookPriority("GJBaseGameLayer::visit", Priority::VeryLate);
    (void)self.setHookPriority("GJBaseGameLayer::flipGravity",
                               Priority::VeryEarly);
    (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects",
                               Priority::VeryEarly);
    (void)self.setHookPriority("GJBaseGameLayer::teleportPlayer",
                               Priority::VeryEarly);
    (void)self.setHookPriority("GJBaseGameLayer::toggleFlipped",
                               Priority::VeryEarly);
  }

  void flipGravity(PlayerObject *player, bool gravity, bool unk) {
    if (g_softToggle) {
      GJBaseGameLayer::flipGravity(player, gravity, unk);
      return;
    }
    if (isFakePlayer(player)) {
      phys::flipGravity(player, gravity);
    } else {
      GJBaseGameLayer::flipGravity(player, gravity, unk);
    }
  }

  void teleportPlayer(TeleportPortalObject *obj, PlayerObject *player) {
    if (g_softToggle) {
      GJBaseGameLayer::teleportPlayer(obj, player);
      return;
    }
    if (isFakePlayer(player)) {
      double yBefore = player->getPositionY();
      phys::teleportPlayer(this, obj, player);
      double yAfter = player->getPositionY();
      m_fields->m_teleportYOffset += (yAfter - yBefore);
    } else {
      GJBaseGameLayer::teleportPlayer(obj, player);
    }
  }

  void collisionCheckObjects(PlayerObject *player,
                             gd::vector<GameObject *> *objects, int length,
                             float dt) {
    if (g_softToggle) {
      GJBaseGameLayer::collisionCheckObjects(player, objects, length, dt);
      return;
    }
    if (isFakePlayer(player)) {
      phys::collisionCheckObjects(this, player, objects, length, dt,
                                  m_fields->m_enableSolidCollisions);
    } else {
      GJBaseGameLayer::collisionCheckObjects(player, objects, length, dt);
    }
  }

  void toggleFlipped(bool flip, bool noEffects) {
    if (g_softToggle) {
      GJBaseGameLayer::toggleFlipped(flip, noEffects);
      return;
    }
    if (g_extrapolating) {
      return;
    }
    GJBaseGameLayer::toggleFlipped(flip, noEffects);
  }

  void update(float dt) override {
    auto playLayer = geode::cast::typeinfo_cast<PlayLayer *>(this);
    bool isPlatformer = (m_player1 && m_player1->m_isPlatformer) ||
                        (m_player2 && m_player2->m_isPlatformer);
    if (g_softToggle || !playLayer || isPlatformer) {
      GJBaseGameLayer::update(dt);
      return;
    }

    m_fields->p1.steps = 0;
    m_fields->p2.steps = 0;

    GJBaseGameLayer::update(dt);

    for (int i = 0; i < 2; ++i) {
      auto &state = (i == 0) ? m_fields->p1 : m_fields->p2;
      auto player = (i == 0) ? m_player1 : m_player2;
      if (player) {
        if (state.steps > 0) {
          state.tickTime = state.lastDt;
          state.lastSteps = state.steps;
        } else {
          state.lastSteps = 0;
        }
      }
    }
  }

  PlayerObject *createFakePlayer(bool isPlayer2) {

    auto player = PlayerObject::create(1, 1, this, this, true);
    if (player) {
      player->retain();
      player->setVisible(false);
      player->m_isSecondPlayer = isPlayer2;
      player->m_playEffects = false;
      this->addChild(player);
    }
    return player;
  }

  void visit() override {
    auto playLayer = geode::cast::typeinfo_cast<PlayLayer *>(this);
    if (!playLayer) {
      GJBaseGameLayer::visit();
      return;
    }

    bool isPlatformer = (m_player1 && m_player1->m_isPlatformer) ||
                        (m_player2 && m_player2->m_isPlatformer);

    bool paused = playLayer->getChildByType<PauseLayer>(0) != nullptr ||
                  CCDirector::sharedDirector()
                          ->getRunningScene()
                          ->getChildByType<PauseLayer>(0) != nullptr;

    bool flipping = playLayer->isFlipping();

    if (g_softToggle || paused || isPlatformer || flipping) {
      GJBaseGameLayer::visit();
      return;
    }

    GJGameState origGameState = m_gameState;
    auto origTweenActions = m_gameState.m_tweenActions;
    auto origCameraOffset = m_gameState.m_cameraOffset;
    auto origCameraZoom = m_gameState.m_cameraZoom;
    auto origCameraAngle = m_gameState.m_cameraAngle;
    auto origCameraPosition = m_gameState.m_cameraPosition;
    bool origResetActiveObjects = m_resetActiveObjects;

    bool hasCBF = !g_cbfSoftToggle || m_clickBetweenSteps;

    bool origPlayerDied = m_playerDied;
    bool hasP1 = m_player1 != nullptr;
    bool hasP2 = m_player2 != nullptr;

    if (m_objectLayer) {
      if (hasP1) {
        if (!m_fields->m_fakePlayer1 ||
            m_fields->m_fakePlayer1->getParent() != this) {
          cleanUpFakePlayer(m_fields->m_fakePlayer1);
          m_fields->m_fakePlayer1 = createFakePlayer(false);
        }
        Bot::get()->trajectory().m_fakePlayer1 = m_fields->m_fakePlayer1;
        Bot::get()->trajectory().unsafeInner()->m_fakePlayer1 =
            m_fields->m_fakePlayer1;
      }
      if (hasP2) {
        if (!m_fields->m_fakePlayer2 ||
            m_fields->m_fakePlayer2->getParent() != this) {
          cleanUpFakePlayer(m_fields->m_fakePlayer2);
          m_fields->m_fakePlayer2 = createFakePlayer(true);
        }
        Bot::get()->trajectory().m_fakePlayer2 = m_fields->m_fakePlayer2;
        Bot::get()->trajectory().unsafeInner()->m_fakePlayer2 =
            m_fields->m_fakePlayer2;
      }
    }
    Bot::get()->trajectory().deactivateAllRemembered();

    CCPoint origP1 = {0, 0};
    CCPoint origP1Rob = {0, 0};
    bool simulatedP1 = false;

    CCPoint origP2 = {0, 0};
    CCPoint origP2Rob = {0, 0};
    bool simulatedP2 = false;

    CCPoint origObj = {0, 0};
    CCPoint camOff = {0, 0};
    bool hasObj = m_objectLayer != nullptr;

    float origObjScaleX = m_objectLayer ? m_objectLayer->getScaleX() : 1.f;
    float origObjScaleY = m_objectLayer ? m_objectLayer->getScaleY() : 1.f;
    float origP1ScaleX = m_player1 ? m_player1->getScaleX() : 1.f;
    float origP1ScaleY = m_player1 ? m_player1->getScaleY() : 1.f;
    float origP2ScaleX = m_player2 ? m_player2->getScaleX() : 1.f;
    float origP2ScaleY = m_player2 ? m_player2->getScaleY() : 1.f;
    float origGroundScaleX = m_groundLayer ? m_groundLayer->getScaleX() : 1.f;
    float origGroundScaleY = m_groundLayer ? m_groundLayer->getScaleY() : 1.f;
    float origGround2ScaleX =
        m_groundLayer2 ? m_groundLayer2->getScaleX() : 1.f;
    float origGround2ScaleY =
        m_groundLayer2 ? m_groundLayer2->getScaleY() : 1.f;
    float origGroundX = m_groundLayer ? m_groundLayer->getPositionX() : 0.f;
    float origGroundY = m_groundLayer ? m_groundLayer->getPositionY() : 0.f;
    float origGround2X = m_groundLayer2 ? m_groundLayer2->getPositionX() : 0.f;
    float origGround2Y = m_groundLayer2 ? m_groundLayer2->getPositionY() : 0.f;
    GroundState groundState1 = saveGroundState(m_groundLayer);
    GroundState groundState2 = saveGroundState(m_groundLayer2);

    std::vector<SavedNodeState> savedGroundChildren1;
    std::vector<SavedNodeState> savedGroundChildren2;
    std::vector<SavedNodeState> savedMiddleground;

    saveNodePositionsRecursive(m_groundLayer, savedGroundChildren1);
    saveNodePositionsRecursive(m_groundLayer2, savedGroundChildren2);
    saveNodePositionsRecursive(m_middleground, savedMiddleground);

    float origObjRot = m_objectLayer ? m_objectLayer->getRotation() : 0.f;
    float origGroundRot = m_groundLayer ? m_groundLayer->getRotation() : 0.f;
    float origGround2Rot = m_groundLayer2 ? m_groundLayer2->getRotation() : 0.f;

    if (hasObj) {
      origObj = m_objectLayer->getPosition();
    }

    float origInShaderObjScaleX =
        m_inShaderObjectLayer ? m_inShaderObjectLayer->getScaleX() : 1.f;
    float origInShaderObjScaleY =
        m_inShaderObjectLayer ? m_inShaderObjectLayer->getScaleY() : 1.f;
    float origInShaderObjRot =
        m_inShaderObjectLayer ? m_inShaderObjectLayer->getRotation() : 0.f;
    CCPoint origInShaderObjPos = m_inShaderObjectLayer
                                     ? m_inShaderObjectLayer->getPosition()
                                     : CCPoint{0, 0};

    float origAboveShaderObjScaleX =
        m_aboveShaderObjectLayer ? m_aboveShaderObjectLayer->getScaleX() : 1.f;
    float origAboveShaderObjScaleY =
        m_aboveShaderObjectLayer ? m_aboveShaderObjectLayer->getScaleY() : 1.f;
    float origAboveShaderObjRot = m_aboveShaderObjectLayer
                                      ? m_aboveShaderObjectLayer->getRotation()
                                      : 0.f;
    CCPoint origAboveShaderObjPos =
        m_aboveShaderObjectLayer ? m_aboveShaderObjectLayer->getPosition()
                                 : CCPoint{0, 0};

    CCPoint origBgPos = {0, 0};
    float origBgScaleX = 1.0f;
    float origBgScaleY = 1.0f;
    float origBgRot = 0.0f;
    bool hasBg = m_background != nullptr;
    if (hasBg) {
      origBgPos = m_background->getPosition();
      origBgScaleX = m_background->getScaleX();
      origBgScaleY = m_background->getScaleY();
      origBgRot = m_background->getRotation();
    }

    float xSign = (hasObj && m_objectLayer->getScaleX() < 0) ? -1 : 1;
    bool dead = m_playerDied;

    double visitAdvance = -1.0; // dont touch this
    auto renderAdvanceSeconds = [&]() -> double {
      if (visitAdvance >= 0.0) {
        return visitAdvance; // exit early when computed once, this lambda got
                             // called 3 times in a single frame (p1, p2,
                             // camera), we dont want adaptive and cpu-smooth
                             // filter be done 3 times in a frame.
      }
      // our own wall timer delta so speedhack can't skew the
      // measurement or the logged fps.
      double now = getCurrentTimestamp();
      double wallDt = m_fields->m_lastVisitTime > 0.0
                          ? now - m_fields->m_lastVisitTime
                          : 1.0 / 60.0;
      if (!std::isfinite(wallDt) || wallDt <= 0.0) {
        wallDt = 1.0 / 60.0;
      }
      if (wallDt > 0.25) {
        wallDt = 0.25;
      }
      m_fields->m_lastVisitTime = now;
      double timeWarp =
          std::isfinite(m_gameState.m_timeWarp) ? m_gameState.m_timeWarp : 1.0;
      if (timeWarp <= 0.0) {
        timeWarp = 1.0;
      }
      double quantum = std::min(timeWarp, 1.0) / 240.0;
      auto clampedAdvance = [&](double advance) -> double {
        if (!std::isfinite(advance) || advance < 0.0) {
          return 0.0;
        }
        if (advance > quantum) {
          return quantum;
        }
        return advance;
      };

      // effective game/wall rate, a speedhack or timewarp scales it
      double scale = m_gameState.m_timeWarp;
      if (m_fields->p1.prevTime > 0.0001 &&
          m_fields->p1.lastTime > m_fields->p1.prevTime &&
          m_fields->p1.lastDt > 0.0001f) {
        double diff = m_fields->p1.lastTime - m_fields->p1.prevTime;
        if (diff > 0.001) {
          scale = (m_fields->p1.lastDt / 60.0f) / diff;
        }
      }
      if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
      }

      switch (g_renderLead) {
      // this would sometimes add a lead because the engine rounds up
      case RenderLead::None:
        visitAdvance = clampedAdvance(m_extraDelta);
        break;

      case RenderLead::Adaptive: {
        // bias half life, might need to finetune this
        constexpr double kBiasHalfLife = 0.2;
        if (!m_fields->m_minRemainderInit) {
          m_fields->m_minRemainderInit = true;
          m_fields->m_minRemainder = 0.0;
        }
        double decay = 0.0;
        if (kBiasHalfLife > 0.0) {
          decay = std::exp2(-wallDt / kBiasHalfLife);
        }
        m_fields->m_minRemainder =
            std::min(m_fields->m_minRemainder * decay, m_extraDelta);
        double bias =
            std::max(0.0, std::min(-m_fields->m_minRemainder, quantum));
        visitAdvance = clampedAdvance(m_extraDelta + bias);
        break;
      }

      case RenderLead::Constant:
        visitAdvance = clampedAdvance(m_extraDelta + 0.5 * quantum);
        break;

      // 2.3.6 behavior with the lag low-passed, so the lead moves in
      // microseconds per frame instead of jumping with the wall measurement.
      // needs more testing, it might not work on higher fps.
      case RenderLead::CpuSmooth: {
        constexpr double kLagHalfLife = 1.0;
        double lagInst = (now - m_fields->p1.lastTime) * scale - m_extraDelta;
        if (!std::isfinite(lagInst)) {
          lagInst = 0.0;
        }
        if (!m_fields->m_lagSlowInit) {
          m_fields->m_lagSlowInit = true;
          m_fields->m_lagSlow = lagInst;
        }
        m_fields->m_lagSlow += (lagInst - m_fields->m_lagSlow) *
                               (1.0 - std::exp2(-wallDt / kLagHalfLife));
        visitAdvance =
            clampedAdvance(m_extraDelta + std::max(0.0, m_fields->m_lagSlow));
        break;
      }

      // 2.3.6 stepping behavior
      case RenderLead::Cpu:
        visitAdvance = clampedAdvance((now - m_fields->p1.lastTime) * scale);
        break;
      }

      if (g_leadLog) {
        // lead against the engine's clock, so the engine running ahead shows
        // up too. this goes to negative on cpu mode sometimes lol
        double lead = visitAdvance - m_extraDelta;
        if (m_fields->m_leadWindowStart == 0.0) {
          m_fields->m_leadWindowStart = now;
        }
        if (m_fields->m_leadCount == 0) {
          m_fields->m_leadMin = lead;
          m_fields->m_leadMax = lead;
        } else {
          m_fields->m_leadMin = std::min(m_fields->m_leadMin, lead);
          m_fields->m_leadMax = std::max(m_fields->m_leadMax, lead);
        }
        m_fields->m_leadSum += lead;
        m_fields->m_leadDtSum += wallDt;
        m_fields->m_leadCount++;
        if (now - m_fields->m_leadWindowStart >= 5.0) {
          double fps = m_fields->m_leadDtSum > 0.0
                           ? m_fields->m_leadCount / m_fields->m_leadDtSum
                           : 0.0;
          log::info("[extrapolate] lead ({}) avg {:.2f} ms, min {:.2f} ms, max "
                    "{:.2f} ms over {:.1f} s ({} samples, {:.0f} fps)",
                    renderLeadName(g_renderLead),
                    m_fields->m_leadSum / m_fields->m_leadCount * 1000.0,
                    m_fields->m_leadMin * 1000.0, m_fields->m_leadMax * 1000.0,
                    now - m_fields->m_leadWindowStart, m_fields->m_leadCount,
                    fps);
          m_fields->m_leadWindowStart = now;
          m_fields->m_leadSum = 0.0;
          m_fields->m_leadDtSum = 0.0;
          m_fields->m_leadCount = 0;
        }
      } else {
        m_fields->m_leadWindowStart = 0.0;
        m_fields->m_leadSum = 0.0;
        m_fields->m_leadDtSum = 0.0;
        m_fields->m_leadCount = 0;
      }

      return visitAdvance;
    };

    // length of one engine step, in the same units as renderAdvanceSeconds() /
    // timeScale.
    auto renderStepSeconds = [&](double timeScale) -> double {
      double timeWarp =
          std::isfinite(m_gameState.m_timeWarp) ? m_gameState.m_timeWarp : 1.0;
      if (timeWarp <= 0.0) {
        timeWarp = 1.0;
      }
      return (std::min(timeWarp, 1.0) / 240.0) / timeScale;
    };

    auto extrapolatePlayer =
        [&](PlayerObject *player, PlayerState &state,
            const std::vector<PlayerButtonCommand> &pendingClicks,
            double tCurrent, double timeScale) {
          double dtSeconds = tCurrent - state.lastTime;
          if (dtSeconds < 0.0)
            dtSeconds = 0.0;

          std::vector<PlayerButtonCommand> sortedClicks = pendingClicks;
          if (g_cbfSoftToggle && m_clickBetweenSteps) {
            double stepDuration = (0.25 / 60.0) / timeScale;
            for (auto &cmd : sortedClicks) {
              double elapsed = cmd.m_timestamp - state.lastTime;
              if (elapsed < 0.0)
                elapsed = 0.0;
              int stepIndex = static_cast<int>(elapsed / stepDuration);
              cmd.m_timestamp =
                  state.lastTime + (stepIndex + 0.5) * stepDuration;
            }
          }
          std::sort(
              sortedClicks.begin(), sortedClicks.end(),
              [](const PlayerButtonCommand &a, const PlayerButtonCommand &b) {
                return a.m_timestamp < b.m_timestamp;
              });

          g_extrapolating = true;

          double currentTime = state.lastTime;
          double targetTime = state.lastTime + dtSeconds;
          state.isDead = false;

          // one step right before the game render, exactly to the segment that
          // ends at the next input (or at the render sample)
          auto updatePlayerSubstepped = [&](double dtFrames) {
            if (dtFrames <= 0.0) {
              return;
            }

            float delta = static_cast<float>(dtFrames);

            m_fields->m_enableSolidCollisions = true;

            player->m_playEffects = false;

            if (player->m_collisionLogTop)
              player->m_collisionLogTop->removeAllObjects();
            if (player->m_collisionLogBottom)
              player->m_collisionLogBottom->removeAllObjects();
            if (player->m_collisionLogLeft)
              player->m_collisionLogLeft->removeAllObjects();
            if (player->m_collisionLogRight)
              player->m_collisionLogRight->removeAllObjects();

            int origNoAutoJump = player->m_stateNoAutoJump;
            int origDartSlide = player->m_stateDartSlide;
            int origHitHead = player->m_stateHitHead;
            int origFlipGravity = player->m_stateFlipGravity;

            player->update(delta);

            player->m_stateNoAutoJump = origNoAutoJump;
            player->m_stateDartSlide = origDartSlide;
            player->m_stateHitHead = origHitHead;
            player->m_stateFlipGravity = origFlipGravity;

            float yBefore = player->getPositionY();
            double yVelBefore = player->m_yVelocity;
            m_fields->m_teleportYOffset = 0.0;

            this->checkCollisions(player, delta, true);
            phys::checkSpawnObjects(this, player);
            if (!player->m_isOnSlope && player->m_stateDartSlide <= 0) {
              float yAfter = player->getPositionY();
              float pushOutY = yAfter - yBefore - m_fields->m_teleportYOffset;

              if (player->m_lastCollisionLeft > 0 ||
                  player->m_lastCollisionRight > 0) {
                if (pushOutY > 0.01f && yVelBefore > 0.05) {
                  float targetY = yBefore + m_fields->m_teleportYOffset;
                  player->setPositionY(targetY);
                  player->m_position.y = targetY;
                  player->m_yVelocity = yVelBefore;
                } else if (pushOutY < -0.01f && yVelBefore < -0.05) {
                  float targetY = yBefore + m_fields->m_teleportYOffset;
                  player->setPositionY(targetY);
                  player->m_position.y = targetY;
                  player->m_yVelocity = yVelBefore;
                }
              }
            }

            player->m_isDead = false;
          };

          for (const auto &cmd : sortedClicks) {
            if (cmd.m_timestamp > currentTime && cmd.m_timestamp < targetTime) {
              double dt = (cmd.m_timestamp - currentTime) * timeScale;
              double dtFrames = dt * 60.0;

              updatePlayerSubstepped(dtFrames);

              currentTime = cmd.m_timestamp;

              if (cmd.m_isPush) {
                extrapolatePushButton(player, cmd.m_button);
              } else {
                extrapolateReleaseButton(player, cmd.m_button);
              }
            }
          }

          if (targetTime > currentTime) {
            double dt = (targetTime - currentTime) * timeScale;
            double dtFrames = dt * 60.0;

            updatePlayerSubstepped(dtFrames);
          }

          player->m_isDead = false;

          g_extrapolating = false;
        };

    if (hasP1 && m_fields->m_fakePlayer1) {
      auto &state = m_fields->p1;
      if (state.lastTime != 0 && !dead) {
        double timeScale = m_gameState.m_timeWarp;
        if (state.prevTime > 0.0001 && state.lastTime > state.prevTime &&
            state.lastDt > 0.0001f) {
          double diff = state.lastTime - state.prevTime;
          if (diff > 0.001) {
            timeScale = (state.lastDt / 60.0f) / diff;
          }
        }
        if (!std::isfinite(timeScale) || timeScale <= 0.0) {
          timeScale = 1.0;
        }

        double dtSeconds = renderAdvanceSeconds() / timeScale;
        double tCurrentClamped = state.lastTime + dtSeconds;

        if (dtSeconds >= 0.0 && dtSeconds < 2.0) {
          std::vector<PlayerButtonCommand> pendingClicks;
          if (hasCBF) {
            bool isTwoPlayer =
                m_levelSettings && m_levelSettings->m_twoPlayerMode;
            if (!collectEarlyClicks(
                    pendingClicks, tCurrentClamped, state.lastTime, dtSeconds,
                    renderStepSeconds(timeScale), false, isTwoPlayer)) {
              for (const auto &cmd : m_queuedButtons) {
                bool isTarget = !cmd.m_isPlayer2 || !isTwoPlayer;
                if (isTarget && cmd.m_timestamp > state.lastTime &&
                    cmd.m_timestamp <= tCurrentClamped) {
                  pendingClicks.push_back(cmd);
                }
              }
            }
          }

          syncFakePlayer(m_fields->m_fakePlayer1, m_player1);

          origP1 = m_player1->getPosition();
          origP1Rob = m_player1->m_position;

          simulatedP1 = true;

          extrapolatePlayer(m_fields->m_fakePlayer1, state, pendingClicks,
                            tCurrentClamped, timeScale);

          auto fakePos = m_fields->m_fakePlayer1->getPosition();
          auto fakeRobPos = m_fields->m_fakePlayer1->m_position;
          if (m_player1->m_stateDartSlide > 0 && !m_player1->m_isOnSlope) {
            fakePos.y = origP1.y;
            fakeRobPos.y = origP1Rob.y;
          }
          m_player1->CCNode::setPosition(fakePos);
          m_player1->m_position = fakeRobPos;
        }
      }
    }

    if (hasP2 && m_fields->m_fakePlayer2 && m_gameState.m_isDualMode) {
      auto &state = m_fields->p2;
      if (state.lastTime != 0 && !dead) {
        double timeScale = m_gameState.m_timeWarp;
        if (state.prevTime > 0.0001 && state.lastTime > state.prevTime &&
            state.lastDt > 0.0001f) {
          double diff = state.lastTime - state.prevTime;
          if (diff > 0.001) {
            timeScale = (state.lastDt / 60.0f) / diff;
          }
        }
        if (!std::isfinite(timeScale) || timeScale <= 0.0) {
          timeScale = 1.0;
        }

        double dtSeconds = renderAdvanceSeconds() / timeScale;
        double tCurrentClamped = state.lastTime + dtSeconds;

        if (dtSeconds >= 0.0 && dtSeconds < 2.0) {
          std::vector<PlayerButtonCommand> pendingClicks;
          if (hasCBF) {
            bool isTwoPlayer =
                m_levelSettings && m_levelSettings->m_twoPlayerMode;
            if (!collectEarlyClicks(
                    pendingClicks, tCurrentClamped, state.lastTime, dtSeconds,
                    renderStepSeconds(timeScale), true, isTwoPlayer)) {
              for (const auto &cmd : m_queuedButtons) {
                bool isTarget = cmd.m_isPlayer2 || !isTwoPlayer;
                if (isTarget && cmd.m_timestamp > state.lastTime &&
                    cmd.m_timestamp <= tCurrentClamped) {
                  pendingClicks.push_back(cmd);
                }
              }
            }
          }

          syncFakePlayer(m_fields->m_fakePlayer2, m_player2);

          origP2 = m_player2->getPosition();
          origP2Rob = m_player2->m_position;

          simulatedP2 = true;

          extrapolatePlayer(m_fields->m_fakePlayer2, state, pendingClicks,
                            tCurrentClamped, timeScale);

          auto fakePos = m_fields->m_fakePlayer2->getPosition();
          auto fakeRobPos = m_fields->m_fakePlayer2->m_position;
          if (m_player2->m_stateDartSlide > 0 && !m_player2->m_isOnSlope) {
            fakePos.y = origP2.y;
            fakeRobPos.y = origP2Rob.y;
          }
          m_player2->CCNode::setPosition(fakePos);
          m_player2->m_position = fakeRobPos;
        }
      }
    }

    bool cameraExtrapolated = false;
    CameraState camState;

    if (hasObj && !dead && hasP1 && m_fields->p1.lastTime != 0) {
      double advanceSeconds = renderAdvanceSeconds();

      if (advanceSeconds > 0.0) {
        camState = saveCameraState();
        cameraExtrapolated = true;

        double warpedDt = advanceSeconds;
        float dtFloat = static_cast<float>(warpedDt);

        gd::unordered_map<int, GJValueTween> filteredTweens;
        for (const auto &[actionID, tween] : m_gameState.m_tweenActions) {
          if (actionID == 1 || actionID == 2 || actionID == 7 ||
              (actionID >= 10 && actionID <= 19) || actionID == 21 ||
              actionID == 22) {
            filteredTweens[actionID] = tween;
          }
        }
        m_gameState.m_tweenActions = filteredTweens;

        m_gameState.updateTweenActions(dtFloat);

        g_extrapolating = true;
        this->updateCamera(dtFloat);
        g_extrapolating = false;
      }
    }

    GJBaseGameLayer::visit();

    if (cameraExtrapolated) {
      restoreCameraState(camState);
      m_gameState = origGameState;

      if (hasObj) {
        m_objectLayer->setPosition(origObj);
        m_objectLayer->setScaleX(origObjScaleX);
        m_objectLayer->setScaleY(origObjScaleY);
        m_objectLayer->setRotation(origObjRot);
      }
      restoreGroundState(m_groundLayer, groundState1);
      restoreGroundState(m_groundLayer2, groundState2);

      restoreNodePositions(savedGroundChildren1, m_groundLayer);
      restoreNodePositions(savedGroundChildren2, m_groundLayer2);
      restoreNodePositions(savedMiddleground, m_middleground);

      if (hasBg) {
        m_background->setPosition(origBgPos);
        m_background->setScaleX(origBgScaleX);
        m_background->setScaleY(origBgScaleY);
        m_background->setRotation(origBgRot);
      }

      if (m_inShaderObjectLayer) {
        m_inShaderObjectLayer->setPosition(origInShaderObjPos);
        m_inShaderObjectLayer->setScaleX(origInShaderObjScaleX);
        m_inShaderObjectLayer->setScaleY(origInShaderObjScaleY);
        m_inShaderObjectLayer->setRotation(origInShaderObjRot);
      }
      if (m_aboveShaderObjectLayer) {
        m_aboveShaderObjectLayer->setPosition(origAboveShaderObjPos);
        m_aboveShaderObjectLayer->setScaleX(origAboveShaderObjScaleX);
        m_aboveShaderObjectLayer->setScaleY(origAboveShaderObjScaleY);
        m_aboveShaderObjectLayer->setRotation(origAboveShaderObjRot);
      }
    }

    if (hasP1 && simulatedP1) {
      m_player1->CCNode::setPosition(origP1);
      m_player1->m_position = origP1Rob;
    }
    if (hasP2 && simulatedP2) {
      m_player2->CCNode::setPosition(origP2);
      m_player2->m_position = origP2Rob;
    }

    m_playerDied = origPlayerDied;

    m_gameState.m_tweenActions = origTweenActions;
    m_resetActiveObjects = origResetActiveObjects;
    if (!cameraExtrapolated) {
      m_gameState.m_cameraOffset = origCameraOffset;
      m_gameState.m_cameraZoom = origCameraZoom;
      m_gameState.m_cameraAngle = origCameraAngle;
      m_gameState.m_cameraPosition = origCameraPosition;
    }

    m_fields->p1.steps = 0;
    m_fields->p2.steps = 0;
  }
};

static bool isFakePlayer(PlayerObject *player) {
  return Bot::get()->trajectory().isFakePlayer(player);
}

class $modify(MyPlayer, PlayerObject) {
  static void onModify(auto &self) {
    (void)self.setHookPriority("PlayerObject::update", Priority::VeryEarly);
    (void)self.setHookPriorityPre("PlayerObject::playDeathEffect",
                                  Priority::First - 100);
    (void)self.setHookPriority("PlayerObject::ringJump", Priority::VeryEarly);
    (void)self.setHookPriority("PlayerObject::bumpPlayer", Priority::VeryEarly);
    (void)self.setHookPriority("PlayerObject::propellPlayer",
                               Priority::VeryEarly);
    (void)self.setHookPriority("PlayerObject::startDashing",
                               Priority::VeryEarly);
    (void)self.setHookPriority("PlayerObject::spiderTestJumpInternal",
                               Priority::VeryEarly);
#ifdef GEODE_IS_WINDOWS
    (void)self.setHookPriority("PlayerObject::stopDashing",
                               Priority::VeryEarly);
#endif
  }

  void ringJump(RingObject *ring, bool unk) {
    if (g_softToggle) {
      PlayerObject::ringJump(ring, unk);
      return;
    }
    if (isFakePlayer(this)) {
      phys::ringJump(this, ring);
    } else {
      PlayerObject::ringJump(ring, unk);
    }
  }

  void bumpPlayer(float force, int objectType, bool playEffect,
                  GameObject *object) {
    if (g_softToggle) {
      PlayerObject::bumpPlayer(force, objectType, playEffect, object);
      return;
    }
    if (isFakePlayer(this)) {
      phys::bumpPlayer(this, force, objectType, playEffect, object);
    } else {
      PlayerObject::bumpPlayer(force, objectType, playEffect, object);
    }
  }

  void propellPlayer(float force, bool dontPlayEffect, int objectType) {
    if (g_softToggle) {
      PlayerObject::propellPlayer(force, dontPlayEffect, objectType);
      return;
    }
    if (isFakePlayer(this)) {
      phys::propellPlayer(this, force, dontPlayEffect, objectType);
    } else {
      PlayerObject::propellPlayer(force, dontPlayEffect, objectType);
    }
  }

  void startDashing(DashRingObject *obj) {
    if (g_softToggle) {
      PlayerObject::startDashing(obj);
      return;
    }
    if (isFakePlayer(this)) {
      phys::startDashing(this, obj);
    } else {
      PlayerObject::startDashing(obj);
    }
  }

#ifdef GEODE_IS_WINDOWS
  void stopDashing() {
    if (g_softToggle) {
      PlayerObject::stopDashing();
      return;
    }
    if (isFakePlayer(this)) {
      phys::stopDashing(this);
    } else {
      PlayerObject::stopDashing();
    }
  }
#endif

  void playDeathEffect() {
    if (g_softToggle) {
      PlayerObject::playDeathEffect();
      return;
    }
    if (g_extrapolating || isFakePlayer(this)) {
      return;
    }
    PlayerObject::playDeathEffect();
  }

  void update(float dt) override {
    if (g_softToggle || m_isPlatformer) {
      PlayerObject::update(dt);
      return;
    }

    auto gameLayer = this->m_gameLayer;
    MyBGL *myGL = nullptr;
    if (gameLayer && geode::cast::typeinfo_cast<PlayLayer *>(gameLayer)) {
      myGL = static_cast<MyBGL *>(gameLayer);
    }

    if (isFakePlayer(this)) {
      PlayerObject::update(dt);
      this->m_isDead = false;
      return;
    }

    PlayerState *state = nullptr;
    if (myGL) {
      bool isP1 = (this == gameLayer->m_player1);
      state = &(isP1 ? myGL->m_fields->p1 : myGL->m_fields->p2);
    }

    CCPoint posBefore = this->getPosition();
    float rotBefore = this->getRotation();
    CCPoint velBefore = CCPoint(static_cast<float>(this->getCurrentXVelocity()),
                                static_cast<float>(this->m_yVelocity));

    if (state) {
      if (state->steps == 0) {
        state->prevTime = state->lastTime;
        state->lastPos = posBefore;
        state->prevVel = velBefore;
        state->lastRot = rotBefore;
        state->lastDt = 0;
      }
    }

    PlayerObject::update(dt);

    if (state) {
      state->lastTime = getCurrentTimestamp();
      state->lastVel = CCPoint(static_cast<float>(this->getCurrentXVelocity()),
                               static_cast<float>(this->m_yVelocity));
      state->lastDt += dt;
      state->steps++;
    }
  }

  void spiderTestJumpInternal(bool dynamic) {
    if (g_softToggle) {
      PlayerObject::spiderTestJumpInternal(dynamic);
      return;
    }
    if (isFakePlayer(this)) {
      double yBefore = this->getPositionY();
      PlayerObject::spiderTestJumpInternal(dynamic);
      double yAfter = this->getPositionY();
      auto gameLayer = this->m_gameLayer;
      MyBGL *myGL = nullptr;
      if (gameLayer && geode::cast::typeinfo_cast<PlayLayer *>(gameLayer)) {
        myGL = static_cast<MyBGL *>(gameLayer);
      }
      if (myGL) {
        myGL->m_fields->m_teleportYOffset += (yAfter - yBefore);
      }
    } else {
      PlayerObject::spiderTestJumpInternal(dynamic);
    }
  }
};

class $modify(MyPlayLayer, PlayLayer) {
  static void onModify(auto &self) {
    (void)self.setHookPriority("PlayLayer::init", Priority::VeryEarly);
    (void)self.setHookPriorityPre("PlayLayer::destroyPlayer",
                                  Priority::First - 100);
    (void)self.setHookPriority("PlayLayer::resetLevel", Priority::VeryEarly);
    (void)self.setHookPriority("PlayLayer::resetLevelFromStart",
                               Priority::VeryEarly);
    (void)self.setHookPriority("PlayLayer::delayedResetLevel",
                               Priority::VeryEarly);
    (void)self.setHookPriority("PlayLayer::fullReset", Priority::VeryEarly);
  }

  bool init(GJGameLevel *level, bool useReplay, bool dontCreateObjects) {
    if (!PlayLayer::init(level, useReplay, dontCreateObjects))
      return false;

#ifdef GEODE_IS_WINDOWS
    refreshCbfInputBinds(); // keybinds can change between levels
#endif

    return true;
  }

  void resetExtrapolation() {
    auto myGL = static_cast<MyBGL *>(static_cast<GJBaseGameLayer *>(this));
    if (myGL) {
      myGL->m_fields->p1 = PlayerState();
      myGL->m_fields->p2 = PlayerState();
    }
  }

  void destroyPlayer(PlayerObject *player, GameObject *object) override {
    if (g_softToggle) {
      PlayLayer::destroyPlayer(player, object);
      return;
    }
    auto myGL = static_cast<MyBGL *>(static_cast<GJBaseGameLayer *>(this));
    if (myGL) {
      if (player == myGL->m_fields->m_fakePlayer1) {
        return;
      }
      if (player == myGL->m_fields->m_fakePlayer2) {
        return;
      }
    }
    PlayLayer::destroyPlayer(player, object);
  }

  void resetLevel() override {
    PlayLayer::resetLevel();
    if (!g_softToggle) {
      resetExtrapolation();
    }
  }

  void loadFromCheckpoint(CheckpointObject *object) {
    PlayLayer::loadFromCheckpoint(object);
    if (!g_softToggle) {
      resetExtrapolation();
    }
  }

  void resetLevelFromStart() {
    PlayLayer::resetLevelFromStart();
    if (!g_softToggle) {
      resetExtrapolation();
    }
  }

  void delayedResetLevel() {
    PlayLayer::delayedResetLevel();
    if (!g_softToggle) {
      resetExtrapolation();
    }
  }

  void fullReset() {
    PlayLayer::fullReset();
    if (!g_softToggle) {
      resetExtrapolation();
    }
  }
};

class $modify(MyRingObject, RingObject) {
  void spawnCircle() {
    if (g_softToggle) {
      RingObject::spawnCircle();
      return;
    }
    if (g_extrapolating) {
      return;
    }
    RingObject::spawnCircle();
  }
};

class $modify(MyEnhancedGameObject, EnhancedGameObject) {
  void activatedByPlayer(PlayerObject *player) {
    if (g_softToggle) {
      EnhancedGameObject::activatedByPlayer(player);
      return;
    }
    if (isFakePlayer(player)) {
      phys::activateForTrajectory(reinterpret_cast<EffectGameObject *>(this),
                                  player);
    } else {
      EnhancedGameObject::activatedByPlayer(player);
    }
  }
};

class $modify(MyGJGroundLayer, GJGroundLayer) {
  void fadeInGround(float duration) {
    if (g_extrapolating) {
      return;
    }
    GJGroundLayer::fadeInGround(duration);
  }

  void fadeOutGround(float duration) {
    if (g_extrapolating) {
      return;
    }
    GJGroundLayer::fadeOutGround(duration);
  }
};
