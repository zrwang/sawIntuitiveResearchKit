/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-    */
/* ex: set filetype=cpp softtabstop=4 shiftwidth=4 tabstop=4 cindent expandtab: */

/*
  Author(s):  Anton Deguet
  Created on: 2014-11-07

  (C) Copyright 2014 Johns Hopkins University (JHU), All Rights Reserved.

--- begin cisst license - do not edit ---

This software is provided "as is" under an open source license, with
no warranty.  The complete license can be found in license.txt and
http://www.cisst.org/cisst/license.txt.

--- end cisst license ---
*/

// system include
#include <iostream>
#include <time.h>

// cisst
#include <sawIntuitiveResearchKit/mtsIntuitiveResearchKitSUJ.h>
#include <cisstMultiTask/mtsInterfaceProvided.h>
#include <cisstMultiTask/mtsInterfaceRequired.h>
#include <cisstParameterTypes/prmPositionJointGet.h>
#include <cisstParameterTypes/prmPositionCartesianGet.h>
#include <cisstParameterTypes/prmEventButton.h>
#include <cisstRobot/robManipulator.h>

const double MAX_BRAKE_CURRENT = 2.2;
const double MIN_BRAKE_CURRENT = 0.0;

const size_t MUX_ARRAY_SIZE = 6;
const size_t MUX_MAX_INDEX = 11;

CMN_IMPLEMENT_SERVICES_DERIVED_ONEARG(mtsIntuitiveResearchKitSUJ, mtsTaskPeriodic, mtsTaskPeriodicConstructorArg)

class mtsIntuitiveResearchKitSUJArmData
{
public:
    inline mtsIntuitiveResearchKitSUJArmData(const std::string & name,
                                             unsigned int plugNumber,
                                             mtsInterfaceProvided * interfaceProvided):
        mName(name),
        mPlugNumber(plugNumber),
        mStateTable(500, name),
        mStateTableConfiguration(100, name + "Configuration"),
        mStateTableBrakeCurrent(100, name + "BrakeCurrent")
    {
        // brake info
        mClutched = false;
        mBrakeCurrent = 0.0;

        // joints
        mJointGet.SetSize(6);
        mJointGet.Zeros();

        // state table doesn't always advance, only when pots are stable
        mStateTable.SetAutomaticAdvance(false);
        mStateTableConfiguration.SetAutomaticAdvance(false);
        mStateTableBrakeCurrent.SetAutomaticAdvance(false);

        for (size_t potArray = 0; potArray < 2; ++potArray) {
            mVoltages[potArray].SetSize(MUX_ARRAY_SIZE);
            mPositions[potArray].SetSize(MUX_ARRAY_SIZE);
            mVoltageToPositionScales[potArray].SetSize(MUX_ARRAY_SIZE);
            mVoltageToPositionOffsets[potArray].SetSize(MUX_ARRAY_SIZE);
        }

        mPositionJointParam.Position().SetSize(MUX_ARRAY_SIZE);

        mStateTable.AddData(this->mVoltages[0], "Voltages[0]");
        mStateTable.AddData(this->mVoltages[1], "Voltages[1]");
        mStateTable.AddData(this->mPositionJointParam, "PositionJoint");
        mStateTable.AddData(this->mPositionCartesianParam, "PositionCartesian");
        mStateTableConfiguration.AddData(this->mName, "Name");
        mStateTableConfiguration.AddData(this->mSerialNumber, "SerialNumber");
        mStateTableConfiguration.AddData(this->mPlugNumber, "PlugNumber");
        mStateTableConfiguration.AddData(this->mVoltageToPositionOffsets[0], "PrimaryJointOffset");//"GetPrimaryJointOffset");
        mStateTableConfiguration.AddData(this->mVoltageToPositionOffsets[1], "SecondaryJointOffset");//"GetSecondaryJointOffset");
        mStateTableBrakeCurrent.AddData(this->mBrakeCurrent, "BrakeCurrent");

        CMN_ASSERT(interfaceProvided);
        interfaceProvided->AddCommandReadState(mStateTable, mPositionJointParam, "GetPositionJoint");
        interfaceProvided->AddCommandReadState(mStateTableConfiguration, mVoltageToPositionOffsets[0], "GetPrimaryJointOffset");
        interfaceProvided->AddCommandReadState(mStateTableConfiguration, mVoltageToPositionOffsets[1], "GetSecondaryJointOffset");
        interfaceProvided->AddCommandReadState(mStateTable, mPositionCartesianParam, "GetPositionCartesian");
        interfaceProvided->AddCommandReadState(mStateTable, mVoltages[0], "GetVoltagesPrimary");
        interfaceProvided->AddCommandReadState(mStateTable, mVoltages[1], "GetVoltagesSecondary");
        interfaceProvided->AddCommandReadState(mStateTableConfiguration, mName, "GetName");
        interfaceProvided->AddCommandReadState(mStateTableConfiguration, mSerialNumber, "GetSerialNumber");
        interfaceProvided->AddCommandReadState(mStateTableConfiguration, mPlugNumber, "GetPlugNumber");
        interfaceProvided->AddCommandVoid(&mtsIntuitiveResearchKitSUJArmData::RecalibrateOffsets, this, "RecalibrateOffsets");
        interfaceProvided->AddCommandReadState(mStateTableBrakeCurrent, mBrakeCurrent, "GetBrakeCurrent");
        // Events
        interfaceProvided->AddEventWrite(MessageEvents.Status, "Status", std::string(""));
        interfaceProvided->AddEventWrite(MessageEvents.Warning, "Warning", std::string(""));
        interfaceProvided->AddEventWrite(MessageEvents.Error, "Error", std::string(""));
        interfaceProvided->AddEventWrite(MessageEvents.RobotState, "RobotState", std::string(""));
        // Stats
        interfaceProvided->AddCommandReadState(mStateTable, mStateTable.PeriodStats,
                                               "GetPeriodStatistics");
    }

    inline void RecalibrateOffsets(void) {
        mStateTableConfiguration.Start();
        mVoltageToPositionOffsets[0].ElementwiseProductOf(mVoltages[0], mVoltageToPositionScales[0]);
        mVoltageToPositionOffsets[0].NegationSelf();
        mVoltageToPositionOffsets[1].ElementwiseProductOf(mVoltages[1], mVoltageToPositionScales[1]);
        mVoltageToPositionOffsets[1].NegationSelf();
        std::cerr << "Primary offsets:   " << mVoltageToPositionOffsets[0] << std::endl
                  << "Secondary offsets: " << mVoltageToPositionOffsets[1] << std::endl;
        mStateTableConfiguration.Advance();
        MessageEvents.Status(std::string("Offsets recalibrated"));
    }

    inline void ClutchCallback(const prmEventButton & button) {
        if (button.Type() == prmEventButton::PRESSED) {
            mClutched = true;
            MessageEvents.Status(mName.Data + ": clutch button pressed");
        } else {
            mClutched = false;
            MessageEvents.Status(mName.Data + ": clutch button released");
        }
    }

    // name of this SUJ arm (ECM, PSM1, ...)
    mtsStdString mName;
    // serial number
    mtsStdString mSerialNumber;
    // plug on back of controller, 1 to 4
    unsigned int mPlugNumber;

    // state of this SUJ arm
    mtsStateTable mStateTable; // for positions, fairly slow, i.e 12 * delay for a2d
    mtsStateTable mStateTableConfiguration; // changes only at config and if recalibrate
    mtsStateTable mStateTableBrakeCurrent; // changes when requested current changes

    // 2 arrays, one for each set of potentiometers
    vctDoubleVec mVoltages[2];
    vctDoubleVec mPositions[2];
    vctDoubleVec mVoltageToPositionScales[2];
    vctDoubleVec mVoltageToPositionOffsets[2];
    prmPositionJointGet mPositionJointParam;
    prmPositionCartesianGet mPositionCartesianParam;

    // Kinematics
    robManipulator mManipulator;
    vctDoubleVec mJointGet;

    // clutch data
    bool mClutched;
    double mBrakeCurrent;

    // Functions for events
    struct {
        mtsFunctionWrite Status;
        mtsFunctionWrite Warning;
        mtsFunctionWrite Error;
        mtsFunctionWrite RobotState;
    } MessageEvents;
};

mtsIntuitiveResearchKitSUJ::mtsIntuitiveResearchKitSUJ(const std::string & componentName, const double periodInSeconds):
    mtsTaskPeriodic(componentName, periodInSeconds)
{
    Init();
}

mtsIntuitiveResearchKitSUJ::mtsIntuitiveResearchKitSUJ(const mtsTaskPeriodicConstructorArg & arg):
    mtsTaskPeriodic(arg)
{
    Init();
}

void mtsIntuitiveResearchKitSUJ::Init(void)
{
    mCounter = 0;
    mMuxTimer = 0.0;
    mMuxState.SetSize(4);
    mVoltages.SetSize(4);
    mClutchCurrents.SetSize(4);

    // Robot IO
    mtsInterfaceRequired * interfaceRequired = AddInterfaceRequired("RobotIO");
    if (interfaceRequired) {
        interfaceRequired->AddFunction("EnablePower", RobotIO.EnablePower);
        interfaceRequired->AddFunction("DisablePower", RobotIO.DisablePower);
        interfaceRequired->AddFunction("GetEncoderChannelA", RobotIO.GetEncoderChannelA);
        interfaceRequired->AddFunction("GetActuatorAmpStatus", RobotIO.GetActuatorAmpStatus);
        interfaceRequired->AddFunction("SetActuatorCurrent", RobotIO.SetActuatorCurrent);
        interfaceRequired->AddFunction("GetAnalogInputVolts", RobotIO.GetAnalogInputVolts);
    }
    interfaceRequired = AddInterfaceRequired("MuxReset");
    if (interfaceRequired) {
        interfaceRequired->AddFunction("GetValue", MuxReset.GetValue);
        interfaceRequired->AddFunction("SetValue", MuxReset.SetValue);
        interfaceRequired->AddFunction("DownUpDown", MuxReset.DownUpDown);
    }
    interfaceRequired = AddInterfaceRequired("MuxIncrement");
    if (interfaceRequired) {
        interfaceRequired->AddFunction("GetValue", MuxIncrement.GetValue);
        interfaceRequired->AddFunction("SetValue", MuxIncrement.SetValue);
        interfaceRequired->AddFunction("DownUpDown", MuxIncrement.DownUpDown);
    }

    mtsInterfaceProvided * interfaceProvided = AddInterfaceProvided("Robot");
    if (interfaceProvided) {
        // Robot State
        interfaceProvided->AddCommandWrite(&mtsIntuitiveResearchKitSUJ::SetRobotControlState,
                                           this, "SetRobotControlState", std::string(""));
        interfaceProvided->AddCommandRead(&mtsIntuitiveResearchKitSUJ::GetRobotControlState,
                                          this, "GetRobotControlState", std::string(""));
        // Events
        interfaceProvided->AddEventWrite(MessageEvents.Status, "Status", std::string(""));
        interfaceProvided->AddEventWrite(MessageEvents.Warning, "Warning", std::string(""));
        interfaceProvided->AddEventWrite(MessageEvents.Error, "Error", std::string(""));
        interfaceProvided->AddEventWrite(MessageEvents.RobotState, "RobotState", std::string(""));
        // Stats
        interfaceProvided->AddCommandReadState(StateTable, StateTable.PeriodStats,
                                               "GetPeriodStatistics");
    }
}


void mtsIntuitiveResearchKitSUJ::Configure(const std::string & filename)
{
    std::ifstream jsonStream;
    jsonStream.open(filename.c_str());

    Json::Value jsonConfig;
    Json::Reader jsonReader;
    if (!jsonReader.parse(jsonStream, jsonConfig)) {
        CMN_LOG_CLASS_INIT_ERROR << "Configure: failed to parse configuration\n"
                                 << jsonReader.getFormattedErrorMessages();
        return;
    }

    // find all arms, there should be 4 of them
    const Json::Value jsonArms = jsonConfig["arms"];
    if (jsonArms.size() != 4) {
        CMN_LOG_CLASS_INIT_ERROR << "Configure: failed to find 4 SUJ arms" << std::endl;
        return;
    }
    for (unsigned int index = 0; index < jsonArms.size(); ++index) {
        // name
        Json::Value jsonArm = jsonArms[index];
        std::string name = jsonArm["name"].asString();
        unsigned int plugNumber = jsonArm["plug-number"].asInt();
        unsigned int armIndex = plugNumber - 1;

        mtsInterfaceProvided * armInterface = this->AddInterfaceProvided(name);
        // Robot State so GUI widget for each arm can set/get state
        armInterface->AddCommandWrite(&mtsIntuitiveResearchKitSUJ::SetRobotControlState,
                                      this, "SetRobotControlState", std::string(""));
        armInterface->AddCommandRead(&mtsIntuitiveResearchKitSUJ::GetRobotControlState,
                                     this, "GetRobotControlState", std::string(""));
        Arms[armIndex] = new mtsIntuitiveResearchKitSUJArmData(name, plugNumber, armInterface);

        // create a required interface for each arm to handle clutch button
        std::stringstream interfaceName;
        interfaceName << "SUJ-Clutch-" << plugNumber;
        mtsInterfaceRequired * requiredInterface = this->AddInterfaceRequired(interfaceName.str());
        if (requiredInterface) {
            requiredInterface->AddEventHandlerWrite(&mtsIntuitiveResearchKitSUJArmData::ClutchCallback, Arms[armIndex],
                                                    "Button");
        } else {
            CMN_LOG_CLASS_INIT_ERROR << "Configure: can't add required interface for SUJ \""
                                     << name << "\" clutch button because this interface already exists: \""
                                     << interfaceName.str() << "\".  Make sure all arms have a different plug number."
                                     << std::endl;
            return;
        }

        // find serial number and potentiometer configurations
        Arms[armIndex]->mSerialNumber = jsonArm["serial-number"].asString();
        cmnDataJSON<vctDoubleVec>::DeSerializeText(Arms[armIndex]->mVoltageToPositionOffsets[0], jsonArm["primary-offsets"]);
        cmnDataJSON<vctDoubleVec>::DeSerializeText(Arms[armIndex]->mVoltageToPositionOffsets[1], jsonArm["secondary-offsets"]);
        cmnDataJSON<vctDoubleVec>::DeSerializeText(Arms[armIndex]->mVoltageToPositionScales[0], jsonArm["primary-scales"]);
        cmnDataJSON<vctDoubleVec>::DeSerializeText(Arms[armIndex]->mVoltageToPositionScales[1], jsonArm["secondary-scales"]);

        // look for DH
        Arms[armIndex]->mManipulator.LoadRobot(jsonArm["DH"]);

        // mV vs V?
        Arms[armIndex]->mStateTableConfiguration.Start();
        for (size_t potIndex = 0; potIndex < 2; potIndex++) {
            Arms[armIndex]->mVoltageToPositionScales[potIndex].Multiply(1000.0 * 0.9); // 0.9 we assume IO suffers from less voltage loss than ISI controller???????
        }
        Arms[armIndex]->mStateTableConfiguration.Advance();
    }
}

void mtsIntuitiveResearchKitSUJ::Startup(void)
{
    this->SetState(mtsIntuitiveResearchKitArmTypes::DVRK_UNINITIALIZED);
}

void mtsIntuitiveResearchKitSUJ::Run(void)
{
    mCounter++;

    ProcessQueuedEvents();

    GetRobotData();

    switch (mRobotState) {
    case mtsIntuitiveResearchKitArmTypes::DVRK_UNINITIALIZED:
        break;
    case mtsIntuitiveResearchKitArmTypes::DVRK_HOMING_POWERING:
        RunHomingPower();
        break;
    case mtsIntuitiveResearchKitArmTypes::DVRK_READY:
        RunReady();
        break;
    default:
        break;
    }

    RunEvent();
    ProcessQueuedCommands();
}

void mtsIntuitiveResearchKitSUJ::Cleanup(void)
{
    // make sure requested current is back to 0
    vctDoubleVec zero(4, 0.0);
    RobotIO.SetActuatorCurrent(zero);
    // turn off amplifiers
    RobotIO.DisablePower();
}

void mtsIntuitiveResearchKitSUJ::GetRobotData(void)
{
    // every 100 ms all we need is to make
    // 100 ms is to make sure A2D stabilizes
    const double muxCycle = 30.0 * cmn_ms;

    // we can start reporting some joint values after the robot is powered
    if (this->mRobotState > mtsIntuitiveResearchKitArmTypes::DVRK_HOMING_POWERING) {
        const double currentTime = this->StateTable.GetTic();

        // time to toggle
        if (currentTime > mMuxTimer) {
            // pot values should be stable by now, get pots values
            GetAndConvertPotentiometerValues();
            // toggle mux
            mMuxTimer = currentTime + muxCycle;
            if (mMuxIndexExpected == MUX_MAX_INDEX) {
                MuxReset.DownUpDown();
                mMuxIndexExpected = 0;
            } else {
                MuxIncrement.DownUpDown();
                mMuxIndexExpected += 1;
            }
        }
    }
}

void mtsIntuitiveResearchKitSUJ::GetAndConvertPotentiometerValues(void)
{
    // read encoder channel A to get the mux state
    mtsExecutionResult executionResult = RobotIO.GetEncoderChannelA(mMuxState);
    // compute pot index
    mMuxIndex = (mMuxState[0]?1:0) + (mMuxState[1]?2:0) + (mMuxState[2]?4:0) + (mMuxState[3]?8:0);
    if (mMuxIndex != mMuxIndexExpected) {
        CMN_LOG_CLASS_RUN_ERROR << "GetAndConvertPotentiometerValues: mux from IO board: " << mMuxIndex << " expected: " << mMuxIndexExpected << std::endl;
        return;
    }
    // array index, 0 or 1, primary or secondary pots
    const size_t arrayIndex = mMuxIndex / MUX_ARRAY_SIZE; // 0 or 1: mux index 0 to 5 goes to first array of data, 6 to 11 goes to second array
    const size_t indexInArray = mMuxIndex % MUX_ARRAY_SIZE; // pot index in array, 0 to 5
    executionResult = RobotIO.GetAnalogInputVolts(mVoltages);
    mtsIntuitiveResearchKitSUJArmData * arm;
    // for each arm, i.e. SUJ1, SUJ2, SUJ3, ...
    for (size_t armIndex = 0; armIndex < 4; ++armIndex) {
        arm = Arms[armIndex];
        // start stable when reading 1st joint on all arms
        if (mMuxIndex == 0) {
            arm->mStateTable.Start();
        }
        // all 4 analog inputs are send to all 4 arm data structures
        arm->mVoltages[arrayIndex][indexInArray] = mVoltages[armIndex];
        // advance state table when all joints have been read
        if (mMuxIndex == MUX_MAX_INDEX) {
            arm->mPositions[0].Assign(arm->mVoltageToPositionOffsets[0]);
            arm->mPositions[0].AddElementwiseProductOf(arm->mVoltageToPositionScales[0], arm->mVoltages[0]);
            arm->mPositions[1].Assign(arm->mVoltageToPositionOffsets[1]);
            arm->mPositions[1].AddElementwiseProductOf(arm->mVoltageToPositionScales[1], arm->mVoltages[1]);
            // temporary hack to build a vector of positions from pots that seem to work
            arm->mPositionJointParam.Position()[0] = arm->mPositions[1][0];
            arm->mPositionJointParam.Position()[1] = arm->mPositions[0][1];
            arm->mPositionJointParam.Position()[2] = arm->mPositions[0][2];
            arm->mPositionJointParam.Position()[3] = arm->mPositions[1][3];
            arm->mPositionJointParam.Position()[4] = arm->mPositions[0][4];
            arm->mPositionJointParam.Position()[5] = arm->mPositions[0][5];
            // advance this arm state table
            arm->mStateTable.Advance();
        }
    }
#if 0
    // debug code
    for (size_t armIndex = 0; armIndex < 4; ++armIndex) {
        //std::cerr << "Arm " << armIndex << std::endl;
        if (mMuxIndex == MUX_MAX_INDEX) {
            //size_t armIndex = 2;
            std::cerr << "Arm " << armIndex << std::endl;
            //std::cerr << "A " << Arms[armIndex]->mVoltages[0] << std::endl << "B " << Arms[armIndex]->mVoltages[1] << std::endl;
            std::cerr<< Arms[armIndex]->mPositionJointParam.Position() * 180.0 / 3.14159 << std::endl;
        }
    }
    if (mMuxIndex == MUX_MAX_INDEX) {
        std::cerr<<std::endl;
    }
#endif
}

void mtsIntuitiveResearchKitSUJ::SetState(const mtsIntuitiveResearchKitArmTypes::RobotStateType & newState)
{
    CMN_LOG_CLASS_RUN_DEBUG << GetName() << ": SetState: new state " << newState << std::endl;

    switch (newState) {

    case mtsIntuitiveResearchKitArmTypes::DVRK_UNINITIALIZED:
        mRobotState = newState;
        DispatchStatus(this->GetName() + " not initialized");
        break;

    case mtsIntuitiveResearchKitArmTypes::DVRK_HOMING_POWERING:
        mHomingTimer = 0.0;
        mHomingPowerRequested = false;
        mRobotState = newState;
        DispatchStatus(this->GetName() + " powering");
        break;

    case mtsIntuitiveResearchKitArmTypes::DVRK_READY:
        // when returning from manual mode, make sure brakes are not released
        mtsIntuitiveResearchKitSUJArmData * arm;
        for (size_t armIndex = 0; armIndex < 4; ++armIndex) {
            arm = Arms[armIndex];
            arm->mClutched = false;
            arm->mBrakeCurrent = 0.0;
            mPreviousTic = 0.0;
        }
        mRobotState = newState;
        DispatchStatus(this->GetName() + " ready");
        break;

    default:
        break;
    }

    // Emit event with current state
    MessageEvents.RobotState(mtsIntuitiveResearchKitArmTypes::RobotStateTypeToString(this->mRobotState));
}

void mtsIntuitiveResearchKitSUJ::RunHomingPower(void)
{
    const double timeToPower = 3.0 * cmn_s;

    const double currentTime = this->StateTable.GetTic();
    // first, request power to be turned on
    if (!mHomingPowerRequested) {
        mHomingTimer = currentTime;
        // pre-load the boards with zero current
        RobotIO.SetActuatorCurrent(vctDoubleVec(NumberOfJoints, 0.0));
        // enable power and set a flags to move to next step
        RobotIO.EnablePower();
        mHomingPowerRequested = true;
        DispatchStatus(this->GetName() + " power requested");
        return;
    }

    // second, check status
    if (mHomingPowerRequested
        && ((currentTime - mHomingTimer) > timeToPower)) {
        // check power status
        vctBoolVec actuatorAmplifiersStatus(NumberOfJoints);
        RobotIO.GetActuatorAmpStatus(actuatorAmplifiersStatus);
        if (actuatorAmplifiersStatus.All()) {
            DispatchStatus(this->GetName() + " power on");
            this->SetState(mtsIntuitiveResearchKitArmTypes::DVRK_READY);

            // reset mux
            MuxReset.DownUpDown();
            Sleep(10.0 * cmn_ms);
            mMuxIndexExpected = 0;
        } else {
            MessageEvents.Error(this->GetName() + " failed to enable power.");
            this->SetState(mtsIntuitiveResearchKitArmTypes::DVRK_UNINITIALIZED);
        }
    }
}

void mtsIntuitiveResearchKitSUJ::RunReady(void)
{
    double currentTic = this->StateTable.GetTic();
    const double timeDelta = currentTic - mPreviousTic;

    const double brakeCurrentRate = 4.0; // rate = 4 A/s, about 1/2 second to get up/down

    mtsIntuitiveResearchKitSUJArmData * arm;
    for (size_t armIndex = 0; armIndex < 4; ++armIndex) {
        arm = Arms[armIndex];
        // brakes
        if (arm->mClutched) {
            arm->mStateTableBrakeCurrent.Start();
            if (((brakeCurrentRate * timeDelta) + arm->mBrakeCurrent) <= MAX_BRAKE_CURRENT) {
                arm->mBrakeCurrent += brakeCurrentRate * timeDelta;
            } else {
                arm->mBrakeCurrent = MAX_BRAKE_CURRENT;
            }
            arm->mStateTableBrakeCurrent.Advance();
        }
        else {
            if (arm->mBrakeCurrent != MIN_BRAKE_CURRENT) {
                arm->mStateTableBrakeCurrent.Start();
                if ((arm->mBrakeCurrent - (brakeCurrentRate * timeDelta)) >= MIN_BRAKE_CURRENT) {
                    arm->mBrakeCurrent -= brakeCurrentRate * timeDelta;
                } else {
                    arm->mBrakeCurrent = MIN_BRAKE_CURRENT;
                }
                arm->mStateTableBrakeCurrent.Advance();
            }
        }
        mClutchCurrents[armIndex] = -arm->mBrakeCurrent;

        // position
        arm->mJointGet.Assign(arm->mPositionJointParam.Position(),arm->mManipulator.links.size());
        vctFrame4x4<double> position = arm->mManipulator.ForwardKinematics(arm->mJointGet, arm->mManipulator.links.size());
        arm->mPositionCartesianParam.Position().From(position);
    }
    RobotIO.SetActuatorCurrent(mClutchCurrents);
    mPreviousTic = currentTic;
}

void mtsIntuitiveResearchKitSUJ::SetRobotControlState(const std::string & state)
{
    if (state == "Home") {
        SetState(mtsIntuitiveResearchKitArmTypes::DVRK_HOMING_POWERING);
    } else if (state == "Manual") {
        SetState(mtsIntuitiveResearchKitArmTypes::DVRK_MANUAL);
    } else {
        mtsIntuitiveResearchKitArmTypes::RobotStateType stateEnum;
        try {
            stateEnum = mtsIntuitiveResearchKitArmTypes::RobotStateTypeFromString(state);
        } catch (std::exception e) {
            MessageEvents.Error(this->GetName() + ": SUJ unsupported state " + state + ": " + e.what());
            return;
        }
        SetState(stateEnum);
    }
}

void mtsIntuitiveResearchKitSUJ::GetRobotControlState(std::string & state) const
{
    state = mtsIntuitiveResearchKitArmTypes::RobotStateTypeToString(this->mRobotState);
}

void mtsIntuitiveResearchKitSUJ::DispatchStatus(const std::string & message)
{
    MessageEvents.Status(message);
    for (size_t armIndex = 0; armIndex < 4; ++armIndex) {
        Arms[armIndex]->MessageEvents.Status(message);
    }
}