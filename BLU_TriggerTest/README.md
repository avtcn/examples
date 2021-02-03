# Sofeware Trigger and Free Run modes

## Instructions
```
# cd examples/BLU_TriggerTest 
# Enter keys to cause sofeware trigger mode and press Ctrl+C to quit the loop
./TriggerTest3reopen  /dev/video0 --software

# Free Run mode in QT application
# cd examples/BasicV4L2/Build/Make/binary/arm_64bit
./BasicDemo -d /dev/video0 

# Repeat above two actions, the images from camera should be fine.
...
...
...

```
