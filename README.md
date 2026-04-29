# Application 6 - Theme Park Ride Safety Controller

## Project Overview

This project is an ESP32 ESP-IDF FreeRTOS prototype for a Central Florida theme park ride safety controller. The system simulates a ride controller that monitors a ride speed/load sensor, warns the operator when the ride condition approaches an unsafe range, and enters a latched emergency-stop state when the E-STOP button or critical sensor threshold is triggered.

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

## Synchronization and Communication

| Primitive | Used For | Why It Is Needed |
|---|---|---|
| Binary semaphore | ISR to `EmergencyResponse` task | Lets the button ISR stay short while waking the hard emergency task quickly |
| Queue | `SensorMonitor` to `Logger` | Transfers timestamped sensor readings without sharing raw variables unsafely |
| Mutex | UART/serial printing | Prevents multiple tasks from interleaving console output |
| Critical section | Shared `rideState` variable | Protects read/write access from multiple tasks |

## Engineering Analysis Questions

### 1. Scheduler Fit

The scheduler setup fits this ride-control prototype because the hard safety tasks have the highest priorities. The `EmergencyResponse` task runs at priority 5, so it can preempt all other tasks when the E-STOP semaphore is given. The `SensorMonitor` task runs at priority 4 and uses `vTaskDelayUntil()` with a 50 ms period, which keeps the sensor sampling periodic instead of drifting over time. The soft tasks, such as `Logger`, `Heartbeat`, and `Diagnostic`, run at lower priorities, so they should not block the hard safety path. A timestamp pair to cite from the serial monitor should look like: `[ESTOP] Emergency stop handled @ ____ ms | ISR-to-task latency = ____ ms | deadline = 20 ms`; if the latency value is below 20 ms, that proves the hard E-STOP deadline was met in Wokwi.

### 2. Race-Proofing

A race could occur on the shared `rideState` variable because the sensor task, emergency task, heartbeat task, and logger task all use the ride state. The protected lines are inside the helper functions `get_ride_state()` and `set_ride_state()`, where `rideState` is read or written. These accesses are protected using `portENTER_CRITICAL(&rideStateMux)` and `portEXIT_CRITICAL(&rideStateMux)`. This prevents one task from reading the state while another task is updating it. Serial printing is also protected using the `serialMutex`, so messages from the logger, diagnostic task, and emergency task do not overlap in the UART output.

### 3. Worst-Case Spike

The heaviest load tested should be turning the potentiometer near its maximum value while also pressing the E-STOP button. This creates the worst case because the sensor task detects an emergency threshold, the ISR can also trigger the emergency semaphore, and the diagnostic task performs its largest variable workload. The diagnostic task intentionally increases its loop count based on the ADC value, which simulates heavier maintenance processing during high ride load. The important margin is the E-STOP response time compared to the 20 ms hard deadline. For example, if the serial monitor shows an ISR-to-task latency of 2 ms, then the remaining margin is `20 ms - 2 ms = 18 ms` before the hard deadline would slip.

### 4. Design Trade-off

One feature that was simplified was full ride restart/reset behavior after an E-STOP. The system intentionally latches into the `ESTOP` state instead of automatically returning to normal operation. This is the safer choice for a theme park ride because a real emergency stop should require operator inspection and manual reset before the ride can continue. Avoiding automatic reset also keeps the timing behavior more predictable because the system does not need extra restart sequencing, revalidation checks, or multiple recovery states. For this proof of concept, the priority is showing that the hard emergency path responds quickly and safely.

## AI Tool Use

AI assistance was used to help draft the ESP-IDF FreeRTOS project structure, code comments, and README wording. The final code and timing proof should still be verified by running the project in Wokwi and checking the serial timestamps.

