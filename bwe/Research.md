# Tuning the bandwidth estimation filter

The Unscented Kalman Filter has a state of current queue length Q, bandwidth BW, and clock offset CO. 
These three values evolve over time to predict the observed absolute difference between local receive time
and remote transmit time of packets. What makes this a bit sensitive is that higher delay can be explained 
by lower BW, higher CO or larger Q. If any of process noise values has a bad setting, the state 
may start to drift in the wrong direction.

The state units are bits, kbps, ms.
The reason for this is to condition the matrix into having comparable magnitudes in the elements.
There are matrix addition, multiplcation and decomposition that will cause 

## Parameter tuning experiments

### Tuning clock drift

Observation shows that the clock source used in linux has a faster pace than the one used in 
Mac Book and Windows Lenovo. It could be different on other devices. But in any case it is 
easier to detect and compensate for a clock drift where the estimator runs faster. That would cause
negative transmission delays which of course is impossible and can be easily adjusted.

Increasing the co noise in _processNoise(2) position has the effect of causing the CO to drop 
and cause a constant high delay measurement. This is because the noise is symmetric around the 
current state. The solution is to offset the clock noise sigma point on the positive side
and pull the negative sigma point closer to the mean. The cross covariance will tell how 
significant this point is and weigh it in that much. We still have the clock offset noise based on the 
covariance in the model that will test symetrically both positive and negative clock offset.

