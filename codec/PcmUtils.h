#include <cstddef>
#include <cstdint>

namespace codec
{

void makeMono(int16_t* data, size_t samples);
void makeStereo(int16_t* data, size_t samples);

} // namespace codec
