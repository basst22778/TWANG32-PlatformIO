#include "Arduino.h"

class Spawner
{
public:
    void Spawn(int pos, int rate_ms, int speed, int dir, int startOffset_ms);
    void Kill();
    int Alive();
    int _pos;
    int _rate;
    int _sp;
    int _dir;
    unsigned long _lastSpawned;
    int _delayOnce;

private:
    int _alive;
};

// @param pos: The location the enemies with be generated from (0..1000)
// @param rate_ms: The time in milliseconds between each new enemy
// @param speed: How fast they move. Typically 1 to 4.
// @param direction: Directions they go 0=down, 1=towards goal
// @param startOffset_ms: The delay in milliseconds before the first enemy (added to rate, can be negative)
void Spawner::Spawn(int pos, int rate_ms, int speed, int dir, int startOffset_ms)
{
    _pos = pos;
    _rate = rate_ms;
    _sp = speed;
    _dir = dir;
    _lastSpawned = millis();
    _delayOnce = startOffset_ms;
    _alive = 1;
}

void Spawner::Kill()
{
    _alive = 0;
}

int Spawner::Alive()
{
    return _alive;
}
