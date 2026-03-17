/**
 * File: BehaviorPowerRobotOff.cpp
 *
 * Author: Kevin M. Karol
 * Created: 2018-07-18
 *
 * Description: Behavior which powers off or reboots the robot
 *
 * Copyright: Anki, Inc. 2018
 *
 **/


#include "engine/aiComponent/behaviorComponent/behaviors/rebootRobot/behaviorPowerRobotOff.h"

#include "cannedAnimLib/cannedAnims/cannedAnimationContainer.h"
#include "clad/robotInterface/messageRobotToEngine.h"
#include "engine/actions/animActions.h"
#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/beiRobotInfo.h"
#include "engine/aiComponent/behaviorComponent/userIntentComponent.h"
#include "engine/aiComponent/behaviorComponent/userIntents.h"
#include "engine/aiComponent/beiConditions/conditions/conditionTimePowerButtonPressed.h"
#include "engine/components/dataAccessorComponent.h"
#include "engine/externalInterface/externalInterface.h"

namespace Anki {
namespace Vector {

namespace{
const char* kPowerButtonActivationKey = "powerButtonHeldToActivate_ms";
const char* kPowerOnAnimName          = "powerOnAnimName";
const char* kPowerOffAnimName         = "powerOffAnimName";
const char* const kWaitForAnimMsgKey  = "waitForAnimMsg";
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorPowerRobotOff::InstanceConfig::InstanceConfig(const Json::Value& config)
{
  const std::string debugName = "BehaviorPowerRobotOff.InstanceConfig.MissingKey. ";
  powerOnAnimName    = JsonTools::ParseString(config, kPowerOnAnimName, debugName + kPowerOnAnimName);
  powerOffAnimName   = JsonTools::ParseString(config, kPowerOffAnimName, debugName + kPowerOffAnimName);

  waitForAnimMsg = config.get( kWaitForAnimMsgKey, false ).asBool();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorPowerRobotOff::DynamicVariables::DynamicVariables()
: waitingForAnimationCallback(false)
, timeLastPowerAnimStopped_ms(0)
, shouldStartPowerOffAnimaiton(false)
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorPowerRobotOff::BehaviorPowerRobotOff(const Json::Value& config)
: ICozmoBehavior(config)
, _iConfig(config)
{
  SubscribeToTags({RobotInterface::RobotToEngineTag::startShutdownAnim});
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
BehaviorPowerRobotOff::~BehaviorPowerRobotOff()
{
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorPowerRobotOff::InitBehavior()
{
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
bool BehaviorPowerRobotOff::WantsToBeActivatedBehavior() const
{
  auto& uic = GetBehaviorComp<UserIntentComponent>();
  return uic.IsUserIntentPending(USER_INTENT(victor_reboot)) || uic.IsUserIntentPending(USER_INTENT(victor_shutdown));
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorPowerRobotOff::GetBehaviorOperationModifiers(BehaviorOperationModifiers& modifiers) const
{
  modifiers.behaviorAlwaysDelegates = false;
  modifiers.wantsToBeActivatedWhenOffTreads = true;
  modifiers.wantsToBeActivatedWhenOnCharger = true;
  modifiers.wantsToBeActivatedWhenCarryingObject = true;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorPowerRobotOff::GetAllDelegates(std::set<IBehavior*>& delegates) const
{
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorPowerRobotOff::GetBehaviorJsonKeys(std::set<const char*>& expectedKeys) const
{
  const char* list[] = {
    kPowerOnAnimName,
    kPowerOffAnimName,
    kPowerButtonActivationKey,
    kWaitForAnimMsgKey,
  };
  expectedKeys.insert( std::begin(list), std::end(list) );
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorPowerRobotOff::OnBehaviorEnteredActivatableScope()
{
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorPowerRobotOff::OnBehaviorActivated()
{
  UserIntentPtr intentDataReboot   = SmartActivateUserIntent(USER_INTENT(victor_reboot));
  UserIntentPtr intentDataShutdown = SmartActivateUserIntent(USER_INTENT(victor_shutdown));

  // reset dynamic variables
  const bool prevShouldStartPowerOffAnimaiton = _dVars.shouldStartPowerOffAnimaiton;
  _dVars = DynamicVariables();

  // make shouldStartPowerOffAnimaiton persist if _iConfig.waitForAnimMsg, since this behavior WantToBeActivated
  // iff shouldStartPowerOffAnimaiton, whenever _iConfig.waitForAnimMsg
  if( _iConfig.waitForAnimMsg ) {
    _dVars.shouldStartPowerOffAnimaiton = prevShouldStartPowerOffAnimaiton;
  }

  if (intentDataShutdown) {
    (void)system("/usr/bin/voff");
  } else if (intentDataReboot) {
    (void)system("/usr/sbin/reboot");
  }

  TransitionToPoweringOff();
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorPowerRobotOff::BehaviorUpdate()
{
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorPowerRobotOff::OnBehaviorLeftActivatableScope()
{
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorPowerRobotOff::AlwaysHandleInScope(const RobotToEngineEvent& event)  {
  if(event.GetData().GetTag() == RobotInterface::RobotToEngineTag::startShutdownAnim){
    _dVars.shouldStartPowerOffAnimaiton = true;
  }
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorPowerRobotOff::TransitionToPoweringOff()
{
  const bool havePlayedAnyAnim = _dVars.timeLastPowerAnimStopped_ms > 0;
  const auto startTime_ms = havePlayedAnyAnim ? GetLengthOfAnimation_ms(_iConfig.powerOffAnimName) - _dVars.timeLastPowerAnimStopped_ms : 0;

  StartAnimation(_iConfig.powerOffAnimName, startTime_ms);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void BehaviorPowerRobotOff::StartAnimation(const std::string& animName, const TimeStamp_t startTime_ms)
{
  _dVars.lastAnimPlayedName = animName;
  const u32 numLoops = 1;
  const bool interruptRunning = true;
  const u8 tracksToLock = (u8)AnimTrackFlag::NO_TRACKS;
  const float timeout_sec = PlayAnimationAction::GetDefaultTimeoutInSeconds();

  auto callback = [this](const AnimationComponent::AnimResult res, u32 streamTimeAnimEnded) {
    _dVars.waitingForAnimationCallback = false;
    if(res == AnimationComponent::AnimResult::Completed){
      _dVars.timeLastPowerAnimStopped_ms = 0;
    }else{
      _dVars.timeLastPowerAnimStopped_ms = streamTimeAnimEnded;
    }
  };
  DelegateIfInControl(new PlayAnimationAction(animName, numLoops, interruptRunning,
                                              tracksToLock, timeout_sec, startTime_ms, callback));

}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TimeStamp_t BehaviorPowerRobotOff::GetLengthOfAnimation_ms(const std::string& animName)
{
  auto& dataAccessorComp = GetBEI().GetComponentWrapper(BEIComponentID::DataAccessor).GetComponent<DataAccessorComponent>();
  const auto* animContainer = dataAccessorComp.GetCannedAnimationContainer();
  auto length_ms = 0;
  if((animContainer != nullptr) && !_iConfig.powerOffAnimName.empty()){
    auto animPtr = animContainer->GetAnimation(_iConfig.powerOffAnimName);
    if(animPtr != nullptr){
      length_ms = animPtr->GetLastKeyFrameEndTime_ms();
    }else{
      PRINT_NAMED_ERROR("BehaviorPowerRobotOff.GetLengthOfAnimation_ms.MissingAnimation",
                        "Animation named %s is not accessible in engine", animName.c_str());
    }
  }

  return length_ms;
}


}
}
