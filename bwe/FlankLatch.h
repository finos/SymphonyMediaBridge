#include <cinttypes>

namespace bwe
{

class FlankLatch
{
public:
    enum Event
    {
        unchanged = 0,
        switchOn,
        switchOff
    };

    FlankLatch() : _levelOn(1), _levelOff(0), _status(false) {}
    FlankLatch(double levelOn, double levelOff) : _levelOn(levelOn), _levelOff(levelOff), _status(false) {}

    void setLevels(double levelOn, double levelOff)
    {
        _levelOn = levelOn;
        _levelOff = levelOff;
    }

    Event update(double value)
    {
        if (_status && value < _levelOff)
        {
            _status = !_status;
            return switchOff;
        }
        else if (!_status && value > _levelOn)
        {
            _status = !_status;
            return switchOn;
        }
        return unchanged;
    }

    bool getStatus() const { return _status; }

private:
    double _levelOn;
    double _levelOff;
    bool _status;
};
} // namespace bwe