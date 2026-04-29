# Application 6 - Theme Park Ride Safety Controller

## Project Overview

This project is an ESP32 FreeRTOS prototype for a Theme Park Ride Safety Controller. The system simulates a ride controller that monitors a ride speed/load sensor, warns the operator when the ride condition approaches an unsafe range, and enters a latched emergency-stop state when the E-STOP button or critical sensor threshold is triggered.

The potentiometer represents a simulated ride speed/load sensor. The green LED is the ride heartbeat, the yellow LED is the warning indicator, and the red LED is the emergency-stop indicator.

## Hardware Mapping

| Component | Wokwi Part | ESP32 Pin | Purpose |
|---|---:|---:|---|
| Green LED | `ledHeartbeat` | GPIO2 | Ride controller heartbeat / alive status |
| Yellow LED | `ledWarning` | GPIO18 | Warning condition indicator |
| Red LED | `ledEmergency` | GPIO19 | Emergency stop indicator |
| Pushbutton | `btnEstop` | GPIO4 | Emergency-stop button input |
| Potentiometer | `potSensor` | GPIO32 / ADC1_CH4 | Simulated ride speed/load sensor |
| UART Serial Monitor | Wokwi serial monitor | TX/RX | Operator/debug output |

## FreeRTOS Task Map

| Task | Priority | Timing Type | Period / Deadline | Ride System Purpose |
|---|---:|---|---|---|
| `EmergencyResponse` | 5 | Hard | Event-driven, 20 ms deadline | Handles E-STOP immediately after ISR/semaphore signal |
| `SensorMonitor` | 4 | Hard | 50 ms period/deadline | Samples simulated ride speed/load and detects warning/emergency thresholds |
| `Heartbeat` | 2 | Soft | 250 ms period/deadline | Blinks green LED to prove the controller is alive |
| `Logger` | 1 | Soft | 200 ms service target | Prints sensor/status data to UART serial monitor |
| `Diagnostic` | 1 | Soft | 1000 ms period/deadline | Simulates variable-time maintenance diagnostics |


## Engineering Analysis Questions

### 1. Scheduler Fit

The scheduler setup fits this ride control design because the hard safety tasks have the highest priorities. The emergency response task runs at priority 5, so it can preempt all other tasks when the E-STOP button is triggered. The sensor monitor task runs at priority 4 and uses `vTaskDelayUntil()` with a 200 ms period, which keeps the sensor sampling periodic instead of drifting over time. The soft tasks, such as logger, heartbeat and diagnostics, run at lower priorities, so they should not block the hard safety path. A timestamp pair to cite from the output should look like: `[ESTOP] handled @ 8408 ms | latency = 0 ms | deadline = 50 ms`; since the latency value is below 50 ms, that proves the hard E-STOP deadline was met.

### 2. Race-Proofing

A race could occur on the shared `rideState` variable because the sensor task, emergency task, heartbeat task, and logger task all use this state. The protected lines are inside the helper functions `get_ride_state()` at lines 109 to 117 and `set_ride_state()` at lines 119 to 126, where `rideState` is read or written. These are protected using `portENTER_CRITICAL(&rideStateMux)` and `portEXIT_CRITICAL(&rideStateMux)` primitives. This prevents one task from reading the state while another task is updating it. Also, serial printing is protected using the `serialMutex`, so messages from the logger, diagnostic task, and emergency task do not overlap in the UART output.

### 3. Worst-Case Spike

The heaviest load I tested was when I maxed out the potentiometer value and spammed the e-stop button at the same time. This creates the worst case because the sensor task detects an emergency threshold, the ISR can also trigger the emergency semaphore, and the diagnostic task performs its largest variable workload. The diagnostic task intentionally increases its loop count based on the ADC value, which simulates heavier maintenance processing during the highest ride conditions. The important margin is the E-STOP response time compared to the 50 ms hard deadline. If the serial monitor shows an ISR to task latency of 7 ms, then the remaining margin would be 43 ms before the hard deadline would slip.

### 4. Design Trade-off

One feature that was simplified was full ride restart/reset behavior after an E-STOP. The system intentionally latches into the `ESTOP` state instead of automatically returning to normal operation. I went with this to line up with what a ride would actually look like, since a true emergency stop would cause for a full system reset for the ride to continue. Avoiding automatic reset also keeps the timing behavior more predictable because the system does not need extra restart sequencing, revalidation checks, or other recovery states.

## AI Tool Use

AI was mainly used to help structure my initial project code. Since this application was essentially pulling all prior applications together, I figured it would be most efficient to use those to my advantage. I gathered some of the prior assignments we did in class that were relevant to my systems needs and had the LLM draft a project skeleton using the code that I had alread written. With the given context of liablilty issues, since these were prior "in-house" used codes, it shouldn't create any liability issues as it isn't another company's resources, rather my own. I also had it help with setting up the markdown file, as I do not like the intricacies of markdown formatting and would rather use the LLM as a resource to do this for me and then fill in the analysis after. Lastly, there were a few logic pieces within the code (where I have inline comments documenting) that I needed some additional help with finding working solutions. I explained what needed to happen in those cases and provided the block of code that it pertained to and had it create the correct logic. As one must do, I verified in testing afterward that it was working as intended, to not blindly rely on the given logic. Lastly, I had it generate the concurrency diagram. I do not have great software to create this myself and am a terrible drawer, so I figured it would be best to have the LLM do this for me.

