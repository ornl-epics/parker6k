/********************************************
 *  p6kController.cpp
 * 
 *  P6K Asyn motor based on the 
 *  asynMotorController class.
 * 
 *  Matt Pearson
 *  26 March 2014
 * 
 ********************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#include <iostream>
using std::cout;
using std::endl;
using std::dec;

#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsExport.h>
#include <epicsString.h>
#include <iocsh.h>
#include <drvSup.h>
#include <registryFunction.h>

#include "asynOctetSyncIO.h"

#include "parker6kController.h"

static const char *driverName = "parker6k";

const epicsUInt32 p6kController::P6K_MAXBUF_ = P6K_MAXBUF;
const epicsFloat64 p6kController::P6K_TIMEOUT_ = 5.0;
const epicsUInt32 p6kController::P6K_ERROR_PRINT_TIME_ = 1; //seconds (this should be set larger when we finish debugging)
const epicsUInt32 p6kController::P6K_FORCED_FAST_POLLS_ = 10;
const epicsUInt32 p6kController::P6K_OK_ = 0;
const epicsUInt32 p6kController::P6K_ERROR_ = 1;

//C function prototypes, for the functions that can be called on IOC shell.
//Some of these functions are provided to ease transition to the model 3 driver. Some of these
//functions could be handled by the parameter library.
extern "C" {
  asynStatus p6kCreateController(const char *portName, const char *lowLevelPortName, int lowLevelPortAddress, 
					 int numAxes, int movingPollPeriod, int idlePollPeriod);
  
  asynStatus p6kCreateAxis(const char *p6kName, int axis);

  asynStatus p6kCreateAxes(const char *p6kName, int numAxes);
  
   
}

/**
 * p6kController constructor.
 * @param portName The Asyn port name to use (that the motor record connects to).
 * @param lowLevelPortName The name of the low level port that has already been created, to enable comms to the controller.
 * @param lowLevelPortAddress The asyn address for the low level port
 * @param numAxes The number of axes on the controller (1 based)
 * @param movingPollPeriod The time (in milliseconds) between polling when axes are moving
 * @param movingPollPeriod The time (in milliseconds) between polling when axes are idle
 */
p6kController::p6kController(const char *portName, const char *lowLevelPortName, int lowLevelPortAddress, 
			       int numAxes, double movingPollPeriod, double idlePollPeriod)
  : asynMotorController(portName, numAxes+1, NUM_MOTOR_DRIVER_PARAMS,
			0, // No additional interfaces
			0, // No addition interrupt interfaces
			ASYN_CANBLOCK | ASYN_MULTIDEVICE, 
			1, // autoconnect
			0, 0)  // Default priority and stack size
{
  static const char *functionName = "p6kController::p6kController";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s Constructor.\n", functionName);

  //Initialize non static data members
  lowLevelPortUser_ = NULL;
  movesDeferred_ = 0;
  nowTimeSecs_ = 0.0;
  lastTimeSecs_ = 0.0;
  printNextError_ = false;

  pAxes_ = (p6kAxis **)(asynMotorController::pAxes_);

  //Create dummy axis for asyn address 0. This is used for controller parameters.
  pAxisZero = new p6kAxis(this, 0);

  //Create controller-specific parameters
  createParam(P6K_C_FirstParamString,       asynParamInt32, &P6K_C_FirstParam_);
  createParam(P6K_C_GlobalStatusString,     asynParamInt32, &P6K_C_GlobalStatus_);
  createParam(P6K_C_CommsErrorString,       asynParamInt32, &P6K_C_CommsError_);
  createParam(P6K_A_DRESString,             asynParamInt32, &P6K_A_DRES_);
  createParam(P6K_A_ERESString,             asynParamInt32, &P6K_A_ERES_);
  createParam(P6K_A_DRIVEString,            asynParamInt32, &P6K_A_DRIVE_);
  createParam(P6K_A_MaxDigitsString,        asynParamInt32, &P6K_A_MaxDigits_);
  createParam(P6K_C_CommandString,          asynParamOctet, &P6K_C_Command_);
  createParam(P6K_A_CommandString,          asynParamOctet, &P6K_A_Command_);
  createParam(P6K_C_FirstParamString,       asynParamInt32, &P6K_C_LastParam_);

  //Connect our Asyn user to the low level port that is a parameter to this constructor
  if (lowLevelPortConnect(lowLevelPortName, lowLevelPortAddress, &lowLevelPortUser_, ">", "\n") != asynSuccess) {
    printf("%s: Failed to connect to low level asynOctetSyncIO port %s\n", functionName, lowLevelPortName);
    setIntegerParam(P6K_C_CommsError_, P6K_ERROR_);
  } else {
    setIntegerParam(P6K_C_CommsError_, P6K_OK_);
  }
  startPoller(movingPollPeriod, idlePollPeriod, P6K_FORCED_FAST_POLLS_);

  bool paramStatus = true;
  paramStatus = ((setIntegerParam(P6K_C_GlobalStatus_, 0) == asynSuccess) && paramStatus);
  paramStatus = ((setStringParam(P6K_C_Command_, " ") == asynSuccess) && paramStatus);

  callParamCallbacks();

  if (!paramStatus) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s Unable To Set Driver Parameters In Constructor.\n", functionName);
  }
 
}


p6kController::~p6kController(void) 
{
  //Destructor. Should never get here.
  delete pAxisZero;
}


/**
 * Connect to the underlying low level Asyn port that is used for comms.
 * This uses the asynOctetSyncIO interface, and also sets the input and output terminators.
 * @param port The port to connect to
 * @param addr The address of the port to connect to
 * @param ppasynUser A pointer to the pasynUser structure used by the controller
 * @param inputEos The input EOS character
 * @param outputEos The output EOS character
 * @return asynStatus  
 */
asynStatus p6kController::lowLevelPortConnect(const char *port, int addr, asynUser **ppasynUser, char *inputEos, char *outputEos)
{
  asynStatus status = asynSuccess;
 
  static const char *functionName = "p6kController::lowLevelPortConnect";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  status = pasynOctetSyncIO->connect( port, addr, ppasynUser, NULL);
  if (status) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
	      "p6kController::motorAxisAsynConnect: unable to connect to port %s\n", 
	      port);
    return status;
  }

  //Do I want to disconnect below? If the IP address comes up, will the driver recover
  //if the poller functions are running? Might have to use asynManager->isConnected to
  //test connection status of low level port (in the pollers). But then autosave 
  //restore doesn't work (and we would save wrong positions). So I need to 
  //have a seperate function(s) to deal with connecting after IOC init.

  status = pasynOctetSyncIO->setInputEos(*ppasynUser, inputEos, strlen(inputEos) );
  if (status) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
	      "p6kController: unable to set input EOS on %s: %s\n", 
	      port, (*ppasynUser)->errorMessage);
    pasynOctetSyncIO->disconnect(*ppasynUser);
    //Set my low level pasynUser pointer to NULL
    *ppasynUser = NULL;
    return status;
  }
  
  status = pasynOctetSyncIO->setOutputEos(*ppasynUser, outputEos, strlen(outputEos));
  if (status) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
	      "p6kController: unable to set output EOS on %s: %s\n", 
	      port, (*ppasynUser)->errorMessage);
    pasynOctetSyncIO->disconnect(*ppasynUser);
    //Set my low level pasynUser pointer to NULL
    *ppasynUser = NULL;
    return status;
  }
  
  return status;
}

/**
 * Utilty function to print the connected status of the low level asyn port.
 * @return asynStatus
 */
asynStatus p6kController::printConnectedStatus()
{
  asynStatus status = asynSuccess;
  int asynManagerConnected = 0;
  static const char *functionName = "p6kController::printConnectedStatus";
  
  if (lowLevelPortUser_) {
    status = pasynManager->isConnected(lowLevelPortUser_, &asynManagerConnected);
      if (status!=asynSuccess) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
		"p6kController: Error calling pasynManager::isConnected.\n");
      return status;
      } else {
	asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s isConnected: %d\n", functionName, asynManagerConnected);
    }
  }
  return status;
}

/**
 * Wrapper for asynOctetSyncIO write/read functions.
 * @param command - String command to send.
 * @response response - String response back.
 */
asynStatus p6kController::lowLevelWriteRead(const char *command, char *response)
{
  asynStatus status = asynSuccess;
  int eomReason = 0;
  size_t nwrite = 0;
  size_t nread = 0;
  int commsError = 0;
  static const char *functionName = "p6kController::lowLevelWriteRead";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);
  
  if (!lowLevelPortUser_) {
    setIntegerParam(P6K_C_CommsError_, P6K_ERROR_);
    return asynError;
  }
  
  asynPrint(lowLevelPortUser_, ASYN_TRACEIO_DRIVER, "%s: command: %s\n", functionName, command);
  
  //Make sure the low level port is connected before we attempt comms
  //Use the controller-wide param P6K_C_CommsError_
  getIntegerParam(P6K_C_CommsError_, &commsError);

  memset(response, 0, sizeof(response));
  
  if (!commsError) {
    status = pasynOctetSyncIO->writeRead(lowLevelPortUser_ ,
					 command, strlen(command),
					 response, P6K_MAXBUF_,
					 P6K_TIMEOUT_,
					 &nwrite, &nread, &eomReason );
    
    if (status) {
      asynPrint(lowLevelPortUser_, ASYN_TRACE_ERROR, "%s: Error from pasynOctetSyncIO->writeRead. command: %s\n", functionName, command);
      setIntegerParam(P6K_C_CommsError_, P6K_ERROR_);
    } else {
      setIntegerParam(P6K_C_CommsError_, P6K_OK_);
    }
  }
  
  asynPrint(lowLevelPortUser_, ASYN_TRACEIO_DRIVER, "%s: response: %s\n", functionName, response); 
  
  return status;
}


void p6kController::report(FILE *fp, int level)
{
  int axis = 0;
  p6kAxis *pAxis = NULL;

  fprintf(fp, "p6k motor driver %s, numAxes=%d, moving poll period=%f, idle poll period=%f\n", 
          this->portName, numAxes_, movingPollPeriod_, idlePollPeriod_);

  if (level > 0) {
    for (axis=0; axis<numAxes_; axis++) {
      pAxis = getAxis(axis);
      if (!pAxis) continue;
      fprintf(fp, "  axis %d\n", 
              pAxis->axisNo_);
    }
  }

  // Call the base class method
  asynMotorController::report(fp, level);
}

/**
 * Deal with controller specific epicsFloat64 params.
 * @param pasynUser
 * @param value
 * @param asynStatus
 */
asynStatus p6kController::writeFloat64(asynUser *pasynUser, epicsFloat64 value)
{
  int function = pasynUser->reason;
  bool status = true;
  p6kAxis *pAxis = NULL;
  char command[P6K_MAXBUF_] = {0};
  char response[P6K_MAXBUF_] = {0};
  double encRatio = 1.0;
  epicsInt32 encposition = 0;
	
  static const char *functionName = "p6kController::writeFloat64";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  pAxis = this->getAxis(pasynUser);
  if (!pAxis) {
    return asynError;
  }

  /* Set the parameter and readback in the parameter library. */
  status = (pAxis->setDoubleParam(function, value) == asynSuccess) && status;

  if (function == motorPosition_) {
    /*Set position on motor axis.*/            
    epicsInt32 position = static_cast<epicsInt32>(floor(value + 0.5));
    
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
	      "%s: Set axis %d on controller %s to position %f\n", 
	      functionName, pAxis->axisNo_, portName, value);

    sprintf(command, "!%dS", pAxis->axisNo_);
    if ( command[0] != 0 && status) {
      status = (lowLevelWriteRead(command, response) == asynSuccess) && status;
    }
    memset(command, 0, sizeof(command));

    sprintf(command, "%dPSET%d", pAxis->axisNo_, position);
    if ( command[0] != 0 && status) {
      status = (lowLevelWriteRead(command, response) == asynSuccess) && status;
    }
    memset(command, 0, sizeof(command));

    /*Now set position on encoder axis.*/
               
    getDoubleParam(motorEncoderRatio_,  &encRatio);
    encposition = (epicsInt32) floor((position*encRatio) + 0.5);
                  
    sprintf(command, "%dPESET%d", pAxis->axisNo_, encposition);
    if ( command[0] != 0 && status) {
      status = (lowLevelWriteRead(command, response) == asynSuccess) && status;
    }
    memset(command, 0, sizeof(command));
    
    /*Now do an update, to get the new position from the controller.*/
    bool moving = true;
    pAxis->getAxisStatus(&moving);
  } 
  else if (function == motorLowLimit_) {
    /* ignore for now, but I think I will need to do:

      epicsInt32 limit = static_cast<epicsInt32>(floor(value + 0.5));

      sprintf(command, "%dLS1", pAxis->axisNo_);
      if ( command[0] != 0 && status) {
         status = (lowLevelWriteRead(command, response) == asynSuccess) && status;
      }
      memset(command, 0, sizeof(command));
      
      sprintf(command, "%dLSNEG", pAxis->axisNo_, limit);
      
     */
    /*asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
    	      "%s: Setting low limit on controller %s, axis %d to %d\n",
    	      functionName, portName, pAxis->axisNo_, limit);*/
  }
  else if (function == motorHighLimit_) {
    /* ignore for now, but I think I will need to do:

      epicsInt32 limit = static_cast<epicsInt32>(floor(value + 0.5));

      sprintf(command, "%dLS1", pAxis->axisNo_);
      if ( command[0] != 0 && status) {
         status = (lowLevelWriteRead(command, response) == asynSuccess) && status;
      }
      memset(command, 0, sizeof(command));
      
      sprintf(command, "%dLSPOS", pAxis->axisNo_, limit);
      
     */
    /*asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
    	      "%s: Setting high limit on controller %s, axis %d to %d\n",
    	      functionName, portName, pAxis->axisNo_, limit);*/
  } 

  if (command[0] != 0 && status) {
    status = (lowLevelWriteRead(command, response) == asynSuccess) && status;
  }

  //Call base class method. This will handle callCallbacks even if the function was handled here.
  status = (asynMotorController::writeFloat64(pasynUser, value) == asynSuccess) && status;

  if (!status) {
    setIntegerParam(pAxis->axisNo_, this->motorStatusCommsError_, P6K_ERROR_);
    return asynError;
  } else {
    setIntegerParam(pAxis->axisNo_, this->motorStatusCommsError_, P6K_OK_);
  }

  return asynSuccess;

}

/**
 * Deal with controller specific epicsInt32 params.
 * @param pasynUser
 * @param value
 * @param asynStatus
 */
asynStatus p6kController::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
  int function = pasynUser->reason;
  //char command[P6K_MAXBUF_] = {0};
  //char response[P6K_MAXBUF_] = {0};
  bool status = true;
  p6kAxis *pAxis = NULL;
  static const char *functionName = "p6kController::writeInt32";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  pAxis = this->getAxis(pasynUser);
  if (!pAxis) {
    return asynError;
  } 

  status = (pAxis->setIntegerParam(function, value) == asynSuccess) && status;


  if (function == motorDeferMoves_) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s: Setting deferred move mode on P6K %s to %d\n", functionName, portName, value);
    if (value == 0 && this->movesDeferred_ != 0) {
      status = (this->processDeferredMoves() == asynSuccess) && status;
    }
    this->movesDeferred_ = value;
  }
  
  //Call base class method. This will handle callCallbacks even if the function was handled here.
  status = (asynMotorController::writeInt32(pasynUser, value) == asynSuccess) && status;
  
  if (!status) {
    setIntegerParam(pAxis->axisNo_, this->motorStatusCommsError_, P6K_ERROR_);
    return asynError;
  } else {
    setIntegerParam(pAxis->axisNo_, this->motorStatusCommsError_, P6K_OK_);
  }

  return asynSuccess;

}


asynStatus p6kController::writeOctet(asynUser *pasynUser, const char *value, 
                                    size_t nChars, size_t *nActual)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    const char *functionName = "parker6kController::writeOctet";

    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s.\n", functionName);
    
    if (function == P6K_C_Command_) {
      //Send command to controller
      cout << functionName << "  Command: " << value << endl;
    } else if (function == P6K_A_Command_) {
      //Send axis specific command to controller. This supports the 
      //primitive commands PREM and POST.
      cout << functionName << "  Axis Command: " << value << endl;
    } else {
      status = asynMotorController::writeOctet(pasynUser, value, nChars, nActual);
    }

    if (status != asynSuccess) {
      callParamCallbacks();
      return asynError;
    }
    
    /* Set the parameter in the parameter library. */
    status = (asynStatus)setStringParam(function, (char *)value);
    /* Do callbacks so higher layers see any changes */
    status = (asynStatus)callParamCallbacks();

    if (status!=asynSuccess) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s Error Setting Parameter. asynUser->reason: %d\n", 
              functionName, function);
    }

    *nActual = nChars;
    return status;
}


/** Returns a pointer to an p6kAxis object.
  * Returns NULL if the axis number encoded in pasynUser is invalid.
  * \param[in] pasynUser asynUser structure that encodes the axis index number. */
p6kAxis* p6kController::getAxis(asynUser *pasynUser)
{
  int axisNo = 0;
    
  getAddress(pasynUser, &axisNo);
  return getAxis(axisNo);
}



/** Returns a pointer to an p6kAxis object.
  * Returns NULL if the axis number is invalid.
  * \param[in] axisNo Axis index number. */
p6kAxis* p6kController::getAxis(int axisNo)
{
  if ((axisNo < 0) || (axisNo >= numAxes_)) return NULL;
  return pAxes_[axisNo];
}


/** 
 * Polls the controller, rather than individual axis.
 * @return asynStatus
 */
asynStatus p6kController::poll()
{
  epicsUInt32 globalStatus = 0;
  bool printErrors = 0;
  bool status = true;
  static const char *functionName = "p6kController::poll";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  if (!lowLevelPortUser_) {
    return asynError;
  }

  /* Get the time and decide if we want to print errors.*/
  epicsTimeGetCurrent(&nowTime_);
  nowTimeSecs_ = nowTime_.secPastEpoch;
  if ((nowTimeSecs_ - lastTimeSecs_) < P6K_ERROR_PRINT_TIME_) {
    printErrors = 0;
  } else {
    printErrors = 1;
    lastTimeSecs_ = nowTimeSecs_;
  }

  if (printNextError_) {
    printErrors = 1;
  }
  
  //Set any controller specific parameters. 
  //Some of these may be used by the axis poll to set axis problem bits.
  status = (getGlobalStatus(&globalStatus) == asynSuccess) && status;
  //status = (setIntegerParam(this->P6K_C_GlobalStatus_, ((globalStatus & P6K_HARDWARE_PROB) != 0)) == asynSuccess) && status;

  /*if (status && feedRatePoll_) {
    status = (setIntegerParam(this->P6K_C_FeedRate_, feedrate) == asynSuccess) && status;
    status = (getIntegerParam(this->P6K_C_FeedRateLimit_, &feedrate_limit) == asynSuccess) && status;
    if (feedrate < static_cast<int>(feedrate_limit-P6K_FEEDRATE_DEADBAND_)) {
      status = (setIntegerParam(this->P6K_C_FeedRateProblem_, P6K_ERROR_) == asynSuccess) && status;
      if (printErrors) {
	asynPrint(lowLevelPortUser_, ASYN_TRACE_ERROR, 
		  "%s: *** ERROR ***: global feed rate below limit. feedrate: %d, feedrate limit %d\n", functionName, feedrate, feedrate_limit);
	printNextError_ = false;
      }
    } else {
      status = (setIntegerParam(this->P6K_C_FeedRateProblem_, P6K_OK_) == asynSuccess) && status;
      printNextError_ = true;
    }
    }*/
  
  callParamCallbacks();

  if (!status) {
    asynPrint(lowLevelPortUser_, ASYN_TRACE_ERROR, "%s: Error reading or setting params.\n", functionName);
    setIntegerParam(P6K_C_CommsError_, P6K_ERROR_);
    return asynError;
  } else {
    setIntegerParam(P6K_C_CommsError_, P6K_OK_);
    return asynSuccess;
  }
}


/**
 * Read the P6K global status integer
 * @param int The global status integer
 * @param int The global feed rate
 */
asynStatus p6kController::getGlobalStatus(epicsUInt32 *globalStatus)
{
  //char command[P6K_MAXBUF_];
  //char response[P6K_MAXBUF_];
  int nvals = 0;
  asynStatus status = asynSuccess;
  static const char *functionName = "p6kController::getGlobalStatus";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);
  /*
  sprintf(command, "???");
  if (lowLevelWriteRead(command, response) != asynSuccess) {
    asynPrint(lowLevelPortUser_, ASYN_TRACE_ERROR, "%s: Error reading ???.\n", functionName);
    status = asynError;
  } else {
    nvals = sscanf(response, "%6x", globalStatus);
    if (nvals != 1) {
      asynPrint(lowLevelPortUser_, ASYN_TRACE_ERROR, "%s: Error reading ???. nvals: %d, response: %s\n", functionName, nvals, response);
      status = asynError;
    } else {
      status = asynSuccess;
    }
  }

  if (feedrate_poll) {
    sprintf(command, "%%");
    if (lowLevelWriteRead(command, response) != asynSuccess) {
      asynPrint(lowLevelPortUser_, ASYN_TRACE_ERROR, "%s: Error reading feedrate.\n", functionName);
      status = asynError;
    } else {
      nvals = sscanf(response, "%d", feedrate);
      if (nvals != 1) {
	asynPrint(lowLevelPortUser_, ASYN_TRACE_ERROR, "%s: Error reading feedrate: nvals: %d, response: %s\n", functionName, nvals, response);
	status = asynError;
      } else {
	status = asynSuccess;
      }
    }
    }*/
  
  if (status == asynSuccess) {
    setIntegerParam(P6K_C_CommsError_, P6K_OK_);
  } else {
    setIntegerParam(P6K_C_CommsError_, P6K_ERROR_);
  }

  return status;

}

/**
 * Disable the check in the axis poller that reads ix24 to check if hardware limits
 * are disabled. By default this is enabled for safety reasons. It sets the motor
 * record PROBLEM bit in MSTA, which results in the record going into MAJOR/STATE alarm.
 * @param axis Axis number to disable the check for.
 */
/*asynStatus p6kController::p6kDisableLimitsCheck(int axis) 
{
  p6kAxis *pA = NULL;
  static const char *functionName = "p6kController::p6kDisableLimitsCheck";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  this->lock();
  pA = getAxis(axis);
  if (pA) {
    pA->limitsCheckDisable_ = 1;
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "%s. Disabling hardware limits disable check on controller %s, axis %d\n", 
              functionName, portName, pA->axisNo_);
  } else {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
	      "%s: Error: axis %d has not been configured using p6kCreateAxis.\n", functionName, axis);
    return asynError;
  }
  this->unlock();
  return asynSuccess;
  }*/

/**
 * Disable the check in the axis poller that reads ix24 to check if hardware limits
 * are disabled. By default this is enabled for safety reasons. It sets the motor
 * record PROBLEM bit in MSTA, which results in the record going into MAJOR/STATE alarm.
 * This function will disable the check for all axes on this controller.
 */
 /*asynStatus p6kController::p6kDisableLimitsCheck(void) 
{
  p6kAxis *pA = NULL;
  static const char *functionName = "p6kController::p6kDisableLimitsCheck";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  this->lock();
  for (int i=0; i<numAxes_; i++) {
    pA = getAxis(i);
    if (!pA) continue;
    pA->limitsCheckDisable_ = 1;
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "%s. Disabling hardware limits disable check on controller %s, axis %d\n", 
              functionName, portName, pA->axisNo_);
  }
  this->unlock();
  return asynSuccess;
  }*/


/**
 * Set the P6K axis scale factor to increase resolution in the motor record.
 * Default value is 1.
 * @param axis Axis number to set the P6K axis scale factor.
 * @param scale Scale factor to set
 */
  /*asynStatus p6kController::p6kSetAxisScale(int axis, int scale) 
{
  p6kAxis *pA = NULL;
  static const char *functionName = "p6kController::p6kSetAxisScale";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  if (scale < 1) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s: Error: scale factor must be >=1.\n", functionName);
    return asynError;
  }

  this->lock();
  pA = getAxis(axis);
  if (pA) {
    pA->scale_ = scale;
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "%s. Setting scale factor of &d on axis %d, on controller %s.\n", 
              functionName, pA->scale_, pA->axisNo_, portName);

  } else {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
	      "%s: Error: axis %d has not been configured using p6kCreateAxis.\n", functionName, axis);
    return asynError;
  }
  this->unlock();
  return asynSuccess;
  }*/


/**
 * If we have an open loop axis that has an encoder coming back on a different channel
 * then the encoder readback axis number can be set here. This ensures that the encoder
 * will be used for the position readback. It will also ensure that the encoder axis
 * is set correctly when performing a set position on the open loop axis.
 *
 * To use this function, the axis number used for the encoder must have been configured
 * already using p6kCreateAxis.
 *
 * @param controller The Asyn port name for the P6K controller.
 * @param axis Axis number to set the P6K axis scale factor.
 * @param encoder_axis The axis number that the encoder is fed into.  
 */
   /*asynStatus p6kController::p6kSetOpenLoopEncoderAxis(int axis, int encoder_axis)
{
  p6kAxis *pA = NULL;
  static const char *functionName = "p6kController::p6kSetOpenLoopEncoderAxis";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  this->lock();
  pA = getAxis(axis);
  if (pA) {
    //Test that the encoder axis has also been configured
    if (getAxis(encoder_axis) == NULL) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
		"%s: Error: encoder axis %d has not been configured using p6kCreateAxis.\n", functionName, encoder_axis);
      return asynError;
    }
    pA->encoder_axis_ = encoder_axis;
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "%s. Setting encoder axis &d for axis %d, on controller %s.\n", 
              functionName, pA->encoder_axis_, pA->axisNo_, portName);

  } else {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
	      "%s: Error: axis %d has not been configured using p6kCreateAxis.\n", functionName, axis);
    return asynError;
  }
  this->unlock();
  return asynSuccess;
  }*/


asynStatus p6kController::processDeferredMoves(void)
{
  asynStatus status = asynSuccess;
  char command[P6K_MAXBUF_] = {0};
  char response[P6K_MAXBUF_] = {0};
  p6kAxis *pAxis = NULL;
  static const char *functionName = "p6kController::processDeferredMoves";

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  //Build up combined move command for all axes involved in the deferred move.
  for (int axis=0; axis<numAxes_; axis++) {
    pAxis = getAxis(axis);
    if (pAxis != NULL) {
      if (pAxis->deferredMove_) {
	//sprintf(command, "%s #%d%s%.2f", command, pAxis->axisNo_,
	//	pAxis->deferredRelative_ ? "J^" : "J=",
	//	pAxis->deferredPosition_);
      }
    }
  }
  
  //Execute the deferred move
  if (lowLevelWriteRead(command, response) != asynSuccess) {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, "%s ERROR Sending Deferred Move Command.\n", functionName);
    setIntegerParam(P6K_C_CommsError_, P6K_ERROR_);
    status = asynError;
  } else {
    setIntegerParam(P6K_C_CommsError_, P6K_OK_);
    status = asynSuccess;
  }

  //Clear deferred move flag for the axes involved.
  for (int axis=0; axis<numAxes_; axis++) {
    pAxis = getAxis(axis);
    if (pAxis!=NULL) {
      if (pAxis->deferredMove_) {
	pAxis->deferredMove_ = 0;
      }
    }
  }
     
  return status;
}





/*************************************************************************************/
/** The following functions have C linkage, and can be called directly or from iocsh */

extern "C" {

/**
 * C wrapper for the p6kController constructor.
 * See p6kController::p6kController.
 *
 */
asynStatus p6kCreateController(const char *portName, const char *lowLevelPortName, int lowLevelPortAddress, 
				int numAxes, int movingPollPeriod, int idlePollPeriod)
{

    p6kController *pp6kController
      = new p6kController(portName, lowLevelPortName, lowLevelPortAddress, numAxes, movingPollPeriod/1000., idlePollPeriod/1000.);
    pp6kController = NULL;

    return asynSuccess;
}

/**
 * C wrapper for the p6kAxis constructor.
 * See p6kAxis::p6kAxis.
 *
 */
asynStatus p6kCreateAxis(const char *p6kName,         /* specify which controller by port name */
			  int axis)                    /* axis number (start from 1). */
{
  p6kController *pC;
  p6kAxis *pAxis;

  static const char *functionName = "p6kCreateAxis";

  pC = (p6kController*) findAsynPortDriver(p6kName);
  if (!pC) {
    printf("%s::%s: ERROR Port %s Not Found.\n",
           driverName, functionName, p6kName);
    return asynError;
  }

  if (axis == 0) {
    printf("%s::%s: ERROR Axis Number 0 Not Allowed. This Asyn Address Is Reserved For Controller Specific Parameters.\n",
	   driverName, functionName);
    return asynError;
  }
  
  pC->lock();
  pAxis = new p6kAxis(pC, axis);
  pAxis = NULL;
  pC->unlock();
  return asynSuccess;
}

/**
 * C Wrapper function for p6kAxis constructor.
 * See p6kAxis::p6kAxis.
 * This function allows creation of multiple p6kAxis objects with axis numbers 1 to numAxes.
 * @param p6kName Asyn port name for the controller (const char *)
 * @param numAxes The number of axes to create, starting at 1.
 *
 */
asynStatus p6kCreateAxes(const char *p6kName,        
			  int numAxes)                   
{
  p6kController *pC;
  p6kAxis *pAxis;

  static const char *functionName = "p6kCreateAxis";

  pC = (p6kController*) findAsynPortDriver(p6kName);
  if (!pC) {
    printf("%s:%s: Error port %s not found\n",
           driverName, functionName, p6kName);
    return asynError;
  }
  
  pC->lock();
  for (int axis=1; axis<=numAxes; axis++) {
    pAxis = new p6kAxis(pC, axis);
    pAxis = NULL;
  }
  pC->unlock();
  return asynSuccess;
}


/**
 * Disable the check in the axis poller that reads ix24 to check if hardware limits
 * are disabled. By default this is enabled for safety reasons. It sets the motor
 * record PROBLEM bit in MSTA, which results in the record going into MAJOR/STATE alarm.
 * @param controller Asyn port name for the controller (const char *)
 * @param axis Axis number to disable the check for.
 * @param allAxes Set to 0 if only dealing with one axis. 
 *                Set to 1 to do all axes (in which case the axis parameter is ignored).
 */
/*asynStatus p6kDisableLimitsCheck(const char *controller, int axis, int allAxes)
{
  p6kController *pC;
  static const char *functionName = "p6kDisableLimitsCheck";

  pC = (p6kController*) findAsynPortDriver(controller);
  if (!pC) {
    printf("%s:%s: Error port %s not found\n",
           driverName, functionName, controller);
    return asynError;
  }

  if (allAxes == 1) {
    return pC->p6kDisableLimitsCheck();
  } else if (allAxes == 0) {
    return pC->p6kDisableLimitsCheck(axis);
  }

  return asynError;
  }*/


/**
 * Set the P6K axis scale factor to increase resolution in the motor record.
 * Default value is 1.
 * @param controller The Asyn port name for the P6K controller.
 * @param axis Axis number to set the P6K axis scale factor.
 * @param scale Scale factor to set
 */
/*asynStatus p6kSetAxisScale(const char *controller, int axis, int scale)
{
  p6kController *pC;
  static const char *functionName = "p6kSetAxisScale";

  pC = (p6kController*) findAsynPortDriver(controller);
  if (!pC) {
    printf("%s:%s: Error port %s not found\n",
           driverName, functionName, controller);
    return asynError;
  }
    
  return pC->p6kSetAxisScale(axis, scale);
  }*/

  
/**
 * If we have an open loop axis that has an encoder coming back on a different channel
 * then the encoder readback axis number can be set here. This ensures that the encoder
 * will be used for the position readback. It will also ensure that the encoder axis
 * is set correctly when performing a set position on the open loop axis.
 *
 * To use this function, the axis number used for the encoder must have been configured
 * already using p6kCreateAxis.
 *
 * @param controller The Asyn port name for the P6K controller.
 * @param axis Axis number to set the P6K axis scale factor.
 * @param encoder_axis The axis number that the encoder is fed into.  
 */
 /*asynStatus p6kSetOpenLoopEncoderAxis(const char *controller, int axis, int encoder_axis)
{
  p6kController *pC;
  static const char *functionName = "p6kSetOpenLoopEncoderAxis";

  pC = (p6kController*) findAsynPortDriver(controller);
  if (!pC) {
    printf("%s:%s: Error port %s not found\n",
           driverName, functionName, controller);
    return asynError;
  }
    
  return pC->p6kSetOpenLoopEncoderAxis(axis, encoder_axis);
  }*/

/* Code for iocsh registration */

/* p6kCreateController */
static const iocshArg p6kCreateControllerArg0 = {"Controller port name", iocshArgString};
static const iocshArg p6kCreateControllerArg1 = {"Low level port name", iocshArgString};
static const iocshArg p6kCreateControllerArg2 = {"Low level port address", iocshArgInt};
static const iocshArg p6kCreateControllerArg3 = {"Number of axes", iocshArgInt};
static const iocshArg p6kCreateControllerArg4 = {"Moving poll rate (ms)", iocshArgInt};
static const iocshArg p6kCreateControllerArg5 = {"Idle poll rate (ms)", iocshArgInt};
static const iocshArg * const p6kCreateControllerArgs[] = {&p6kCreateControllerArg0,
							    &p6kCreateControllerArg1,
							    &p6kCreateControllerArg2,
							    &p6kCreateControllerArg3,
							    &p6kCreateControllerArg4,
							    &p6kCreateControllerArg5};
static const iocshFuncDef configp6kCreateController = {"p6kCreateController", 6, p6kCreateControllerArgs};
static void configp6kCreateControllerCallFunc(const iocshArgBuf *args)
{
  p6kCreateController(args[0].sval, args[1].sval, args[2].ival, args[3].ival, args[4].ival, args[5].ival);
}


/* p6kCreateAxis */
static const iocshArg p6kCreateAxisArg0 = {"Controller port name", iocshArgString};
static const iocshArg p6kCreateAxisArg1 = {"Axis number", iocshArgInt};
static const iocshArg * const p6kCreateAxisArgs[] = {&p6kCreateAxisArg0,
                                                     &p6kCreateAxisArg1};
static const iocshFuncDef configp6kAxis = {"p6kCreateAxis", 2, p6kCreateAxisArgs};

static void configp6kAxisCallFunc(const iocshArgBuf *args)
{
  p6kCreateAxis(args[0].sval, args[1].ival);
}

/* p6kCreateAxes */
static const iocshArg p6kCreateAxesArg0 = {"Controller port name", iocshArgString};
static const iocshArg p6kCreateAxesArg1 = {"Num Axes", iocshArgInt};
static const iocshArg * const p6kCreateAxesArgs[] = {&p6kCreateAxesArg0,
                                                     &p6kCreateAxesArg1};
static const iocshFuncDef configp6kAxes = {"p6kCreateAxes", 2, p6kCreateAxesArgs};

static void configp6kAxesCallFunc(const iocshArgBuf *args)
{
  p6kCreateAxes(args[0].sval, args[1].ival);
}


/* p6kDisableLimitsCheck */
/*static const iocshArg p6kDisableLimitsCheckArg0 = {"Controller port name", iocshArgString};
static const iocshArg p6kDisableLimitsCheckArg1 = {"Axis number", iocshArgInt};
static const iocshArg p6kDisableLimitsCheckArg2 = {"All Axes", iocshArgInt};
static const iocshArg * const p6kDisableLimitsCheckArgs[] = {&p6kDisableLimitsCheckArg0,
							      &p6kDisableLimitsCheckArg1,
							      &p6kDisableLimitsCheckArg2};
static const iocshFuncDef configp6kDisableLimitsCheck = {"p6kDisableLimitsCheck", 3, p6kDisableLimitsCheckArgs};

static void configp6kDisableLimitsCheckCallFunc(const iocshArgBuf *args)
{
  p6kDisableLimitsCheck(args[0].sval, args[1].ival, args[2].ival);
  }*/



/* p6kSetAxisScale */
/*static const iocshArg p6kSetAxisScaleArg0 = {"Controller port name", iocshArgString};
static const iocshArg p6kSetAxisScaleArg1 = {"Axis number", iocshArgInt};
static const iocshArg p6kSetAxisScaleArg2 = {"Scale", iocshArgInt};
static const iocshArg * const p6kSetAxisScaleArgs[] = {&p6kSetAxisScaleArg0,
							      &p6kSetAxisScaleArg1,
							      &p6kSetAxisScaleArg2};
static const iocshFuncDef configp6kSetAxisScale = {"p6kSetAxisScale", 3, p6kSetAxisScaleArgs};

static void configp6kSetAxisScaleCallFunc(const iocshArgBuf *args)
{
  p6kSetAxisScale(args[0].sval, args[1].ival, args[2].ival);
  }*/

/* p6kSetOpenLoopEncoderAxis */
/*static const iocshArg p6kSetOpenLoopEncoderAxisArg0 = {"Controller port name", iocshArgString};
static const iocshArg p6kSetOpenLoopEncoderAxisArg1 = {"Axis number", iocshArgInt};
static const iocshArg p6kSetOpenLoopEncoderAxisArg2 = {"Encoder Axis", iocshArgInt};
static const iocshArg * const p6kSetOpenLoopEncoderAxisArgs[] = {&p6kSetOpenLoopEncoderAxisArg0,
								  &p6kSetOpenLoopEncoderAxisArg1,
								  &p6kSetOpenLoopEncoderAxisArg2};
static const iocshFuncDef configp6kSetOpenLoopEncoderAxis = {"p6kSetOpenLoopEncoderAxis", 3, p6kSetOpenLoopEncoderAxisArgs};

static void configp6kSetOpenLoopEncoderAxisCallFunc(const iocshArgBuf *args)
{
  p6kSetOpenLoopEncoderAxis(args[0].sval, args[1].ival, args[2].ival);
  }*/


static void p6kControllerRegister(void)
{
  iocshRegister(&configp6kCreateController,   configp6kCreateControllerCallFunc);
  iocshRegister(&configp6kAxis,               configp6kAxisCallFunc);
  iocshRegister(&configp6kAxes,               configp6kAxesCallFunc);
  //  iocshRegister(&configp6kDisableLimitsCheck, configp6kDisableLimitsCheckCallFunc);
  //  iocshRegister(&configp6kSetAxisScale, configp6kSetAxisScaleCallFunc);
  // iocshRegister(&configp6kSetOpenLoopEncoderAxis, configp6kSetOpenLoopEncoderAxisCallFunc);
}
epicsExportRegistrar(p6kControllerRegister);

#ifdef vxWorks
  //VxWorks register functions
  epicsRegisterFunction(p6kCreateController);
  epicsRegisterFunction(p6kCreateAxis);
  epicsRegisterFunction(p6kCreateAxes);
//  epicsRegisterFunction(p6kDisableLimitsCheck);
//  epicsRegisterFunction(p6kSetAxisScale);
//  epicsRegisterFunction(p6kSetOpenLoopEncoderAxis);
#endif
} // extern "C"

