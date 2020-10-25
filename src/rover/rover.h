#ifndef ROVER_H
#define ROVER_H

#include "../wheel/drive_wheel.h"

#include <stdint.h>

#ifdef DEBUG
    #include <stdio.h>
    #define LOG(_msg) do{printf(String(_msg).c_str());}while(0)
    #define LOGFMT(_msg, ...) do{printf((String(_msg) + String("\n")).c_str(), __VA_ARGS__);}while(0)
#else
    #define LOG(_msg) do{}while(0)
    #define LOGFMT(_msg, ...) do{}while(0)
#endif

#ifndef SUCCESS
    #define SUCCESS (0)
    #define FAILURE (-1)
#endif


//
// speed/direction command to send to hardware 
// for a single wheel
//
typedef struct PwmCommand {
    bool forward;
    pwm_type value;
} PwmCommand;


//
// speed/direction command to send to hardware 
// for a single wheel
//
typedef float SpeedValue;
typedef struct SpeedCommand {
    SpeedCommand(): forward(false), value(SpeedValue()) {};
    SpeedCommand(bool f, SpeedValue s): forward(f), value(s) {};

    bool forward;
    SpeedValue value;
} SpeedCommand;

//
// command to set speed control parameters
//
typedef struct PidCommand {
    PidCommand(): maxSpeed(0), Kp(0), Ki(0), Kd(0) {};
    PidCommand(SpeedValue m, float p, float i, float d): maxSpeed(m), Kp(p), Ki(i), Kd(d) {};

    SpeedValue maxSpeed;    // maximum measured speed
    float Kp;               // proportional gain
    float Ki;               // integral gain
    float Kd;               // derivative gain
} PidCommand;

//
// command to change speed and direction 
// for both wheels
//
typedef struct TankCommand {
    TankCommand(): useSpeedControl(false), left(false, 0), right(false, 0) {};
    TankCommand(bool u, SpeedCommand l, SpeedCommand r): useSpeedControl(u), left(l), right(r) {};

    bool useSpeedControl;    // true to call setSpeed(), false to call setPower()
    SpeedCommand left;
    SpeedCommand right;
} TankCommand;

//
// discriminate between commands
//
typedef enum {
    NOOP = 0,
    HALT,
    TANK,
    PID
} CommandType;


typedef struct RoverCommand {
    RoverCommand(): type(NOOP), tank(TankCommand()) {};
    RoverCommand(CommandType t, TankCommand tc): type(t), tank(tc) {};
    RoverCommand(CommandType t, PidCommand pc): type(t), pid(pc) {};

    CommandType type;    // if matched, TANK or PID, else NOOP
    union  {
        TankCommand tank;  // if matched, the tank command, else {{true, 0}, {true, 0}}
        PidCommand pid;    // if matched, the pid command, else
    };
} RoverCommand;

typedef struct SubmitCommandResult {
    int status;
    int id;
    RoverCommand command;
} SubmitCommandResult;


#define MAX_SPEED_COMMAND (255)

#define COMMAND_BAD_FAILURE (-1)
#define COMMAND_PARSE_FAILURE (-2)
#define COMMAND_ENQUEUE_FAILURE (-3)


class TwoWheelRover {
    private:

    pwm_type _speedLeft = 0;
    pwm_type _speedRight = 0;
    pwm_type _forwardLeft = 1;
    pwm_type _forwardRight = 1;

    static const unsigned int COMMAND_BUFFER_SIZE = 4;
    TankCommand _commandQueue[COMMAND_BUFFER_SIZE];     // circular queue of commands
    uint8_t _commandHead = 0; // read from head
    uint8_t _commandTail = 0; // append to tail

    DriveWheel *_leftWheel = nullptr;
    DriveWheel *_rightWheel = nullptr;

    unsigned int _lastLeftCount = 0;
    unsigned int _lastRightCount = 0;

    /**
     * Poll command queue 
     */
    TwoWheelRover& _pollRoverCommand(); // RET: this rover

   /**
     * Poll rover wheel encoders
     */
    TwoWheelRover& _pollWheels();   // RET: this rover

    /**
     * send speed and direction to left wheel
     */
    TwoWheelRover& _roverWheelSpeed(
        DriveWheel* wheel,      // IN : the wheel to control
        bool useSpeedControl,   // IN : true to call setSpeed() and enable speed controller
                                //      false to call setPower() and disable speed controller
        bool forward,           // IN : true to move wheel in forward direction
                                //      false to move wheel in reverse direction
        SpeedValue speed);      // IN : target speed for wheel
                                // RET: this TwoWheelRover

    public:

    ~TwoWheelRover() {
        detach();
    }

    /**
     * Deteremine if rover's dependencies are attached
     */
    bool attached();

    /**
     * Attach rover dependencies
     */
    TwoWheelRover& attach(
        DriveWheel &leftWheel,   // IN : left drive wheel in attached state
        DriveWheel &rightWheel); // IN : right drive wheel in attached state
                                 // RET: this rover in attached state

    /**
     * Detach rover dependencies
     */
    TwoWheelRover& detach(); // RET: this rover in detached state

    /**
     * Set speed control parameters
     */
    TwoWheelRover& setSpeedControl(
        speed_type maxSpeed,    // IN : maximum speed of motor
        float Kp,               // IN : proportional gain
        float Ki,               // IN : integral gain
        float Kd);              // IN : derivative gain
                                // RET: this TwoWheelRover

    /**
     * Read value of left wheel encoder
     */
    encoder_count_type readLeftWheelEncoder(); // RET: wheel encoder count

    /**
     * Read value of right wheel encoder
     */
    encoder_count_type readRightWheelEncoder(); // RET: wheel encoder count

    /**
     * Poll rover systems
     */
    TwoWheelRover& poll();   // RET: this rover

    /**
     * Add a command, as string parameters, to the command queue
     */
    int submitTurtleCommand(
        boolean useSpeedControl,    // IN : true if command is a speed command
                                    //      false if command is a pwm command
        const char *directionParam, // IN : direction as a string; "forward",
                                    //      "reverse", "left", "right", or "stop"
        const char *speedParam);    // IN : speed as an numeric string
                                    //      - if useSpeedControl is true, speed >= 0
                                    //      - if useSpeedControl is false, 0 >= speed >= 255
                                    // RET: 0 for SUCCESS, non-zero for error code


    /*
    ** submit the tank command that was
    ** send in the websocket channel
    */
    SubmitCommandResult submitTankCommand(
        const char *commandParam,   // IN : A wrapped tank command link cmd(tank(...))
        const int offset);          // IN : offset of cmd() wrapper in command buffer
                                    // RET: struct with status, command id and command
                                    //      where status == SUCCESS or
                                    //      status == -1 on bad command (null or empty)
                                    //      status == -2 on parse error
                                    //      status == -3 on enqueue error (queue is full)


    /**
     * Append a command to the command queue.
     */
    int enqueueRoverCommand(
        TankCommand command);   // IN : speed/direction for both wheels
                                // RET: SUCCESS if command could be queued
                                //      FAILURE if buffer is full.


    /**
     * Get the next command from the command queue.
     */
    int dequeueRoverCommand(
        TankCommand *command);  // OUT: on SUCCESS, speed/direction for both wheels
                                //      otherwise unchanged.
                                // RET: SUCCESS if buffer had a command to return 
                                //      FAILURE if buffer is empty.


    /**
     * Execute the given rover command
     */
    int executeRoverCommand(
        TankCommand &command);  // IN : speed/direction for both wheels
                                // RET: SUCCESS if command executed
                                //      FAILURE if command could not execute


    /**
     * Immediately 
     * - stop the rover  
     * - disengage speed controller  
     * - clear command queue
     */
    TwoWheelRover& roverHalt();   // RET: this rover

    private: 

    /**
     * send speed and direction to left wheel
     */
    TwoWheelRover& roverLeftWheel(
        bool useSpeedControl,   // IN : true to call setSpeed() and enable speed controller
                                //      false to call setPower() and disable speed controller
        bool forward,           // IN : true to move wheel in forward direction
                                //      false to move wheel in reverse direction
        SpeedValue speed);      // IN : target speed for wheel
                                // RET: this TwoWheelRover


    /**
     * send speed and direction to right wheel
     */
    TwoWheelRover& roverRightWheel(
    bool useSpeedControl,   // IN : true to call setSpeed() and enable speed controller
                            //      false to call setPower() and disable speed controller
    bool forward,           // IN : true to move wheel in forward direction
                            //      false to move wheel in reverse direction
    SpeedValue speed);      // IN : target speed for wheel
                            // RET: this TwoWheelRover

    /**
     * Log the current value of the wheel encoders
     */
    void logWheelEncoders(EncoderLogger logger) {
        #ifdef LOG_MESSAGE
        #ifdef LOG_LEVEL
            #if (LOG_LEVEL >= DEBUG_LEVEL)
                if(NULL != _leftEncoder) {
                    unsigned int thisLeftCount = readLeftWheelEncoder();
                    if(thisLeftCount != _lastLeftCount) {
                        logger("Left Wheel:  ", thisLeftCount);
                        _lastLeftCount = thisLeftCount;
                    }
                }
                if(NULL != _rightEncoder) {
                    unsigned int thisRightCount = readRightWheelEncoder();
                    if(thisRightCount != _lastRightCount) {
                        logger("Right Wheel:  ", thisRightCount);
                        _lastRightCount = thisRightCount;
                    }
                }
            #endif
        #endif
        #endif
}

};

#endif
