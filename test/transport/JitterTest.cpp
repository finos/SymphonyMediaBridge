#include "bwe/BandwidthEstimator.h"
#include "codec/AudioFader.h"
#include "concurrency/MpmcQueue.h"
#include "logger/Logger.h"
#include "logger/PacketLogger.h"
#include "math/Matrix.h"
#include "math/WelfordVariance.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/JitterBufferList.h"
#include "rtp/JitterEstimator.h"
#include "rtp/JitterTracker.h"
#include "rtp/RtpHeader.h"
#include "rtp/SendTimeDial.h"
#include "test/CsvWriter.h"
#include "test/bwe/FakeAudioSource.h"
#include "test/bwe/FakeCall.h"
#include "test/bwe/FakeCrossTraffic.h"
#include "test/bwe/FakeVideoSource.h"
#include "test/transport/NetworkLink.h"
#include <gtest/gtest.h>

using namespace math;

#include "utils/ScopedFileHandle.h"
#include <gtest/gtest.h>
#include <random>

using namespace math;

namespace rtp
{

template <typename T, size_t S>
class Backlog
{
public:
    Backlog() : _index(0) {}
    T add(T value)
    {
        _index = (_index + 1) % S;
        auto prev = _values[_index];
        _values[_index] = value;
        return prev;
    }

    T front() { return _values[_index]; }
    T back() { return _values[(_index + 1) % S]; }

    T getMean() const
    {
        T acc = 0;
        for (int i = 0; i < S; ++i)
        {
            acc += _values[i];
        }

        return acc / S;
    }

    T getVariance(T hypotheticalMean) const
    {
        T acc = 0;
        for (size_t i = 0; i < S; ++i)
        {
            auto d = _values[i] - hypotheticalMean;
            acc += d * d;
        }

        return acc / S;
    }

private:
    T _values[S];
    uint32_t _index;
};
} // namespace rtp
namespace
{

/* clang-format off */
uint32_t normalDistribution [] = {
    592,559,401,471,728,426,479,278,517,241,672,604,502,551,587,391,434,557,626,648,
    443,353,459,392,444,520,535,352,522,587,581,643,403,625,315,558,596,541,473,648,
    478,482,500,565,420,473,300,493,435,556,405,474,308,672,459,419,531,550,541,278,
    633,556,650,481,692,626,455,330,608,759,423,606,412,426,692,552,427,685,610,526,
    451,659,412,567,389,529,466,674,556,416,380,366,600,519,557,553,515,537,468,445,
    397,443,470,535,673,428,378,581,481,522,498,475,544,409,462,405,572,305,447,597,
    635,373,483,420,508,478,520,357,466,625,417,404,624,203,457,432,599,531,667,315,
    553,573,501,471,302,427,379,284,438,440,458,494,540,439,520,522,512,513,489,596,
    433,369,505,388,498,562,594,483,412,462,637,400,639,354,492,629,462,525,504,253,
    657,460,599,490,563,589,451,455,517,547,489,405,477,501,368,434,527,382,456,527,
    514,560,566,510,519,603,513,450,493,566,542,567,570,437,589,464,604,346,532,625,
    404,379,295,595,574,689,589,598,399,564,412,411,671,311,561,737,488,365,554,453,
    459,594,530,393,546,252,392,448,527,506,546,353,519,524,414,480,508,404,530,431,
    476,416,529,439,519,346,398,390,659,667,605,489,418,501,539,524,614,514,443,443,
    625,500,506,636,512,629,543,348,628,516,738,352,553,364,563,536,437,502,600,407,
    494,797,504,530,466,537,574,628,289,794,422,524,484,614,659,358,450,659,456,341,
    564,645,639,611,628,509,672,450,407,367,374,442,609,522,373,380,508,317,391,548,
    494,556,435,360,537,682,511,515,494,543,419,543,226,478,586,622,493,405,642,627,
    737,606,581,416,582,504,629,369,425,322,499,674,468,461,722,370,485,518,565,462,
    450,538,496,539,544,376,469,550,575,517,517,354,397,473,469,631,345,493,680,271,
    518,383,660,254,491,411,442,500,630,698,475,462,804,479,491,522,574,430,415,406,
    362,569,614,413,638,499,620,451,539,444,372,497,612,432,446,340,530,395,492,428,
    520,224,548,524,613,507,571,580,532,513,384,613,499,447,443,482,466,454,371,588,
    597,422,470,533,513,501,426,508,641,417,619,456,435,240,574,461,510,593,596,468,
    399,487,704,480,347,549,547,447,608,653,567,536,328,552,439,418,446,428,665,517,
    424,580,732,465,572,499,672,500,518,483,662,626,651,393,565,596,535,442,479,486,
    594,659,655,520,608,572,476,418,476,606,516,449,723,559,361,542,560,705,485,623,
    665,615,450,474,476,657,606,598,570,483,343,621,371,475,276,551,601,589,592,664,
    499,432,485,442,683,396,439,608,618,462,502,557,564,495,360,685,477,606,444,495,
    507,581,634,399,382,434,620,629,433,485,490,444,462,597,265,264,435,684,407,481,
    411,457,384,343,454,424,471,526,641,419,435,483,578,615,402,522,500,479,521,430,
    629,634,489,927,661,621,559,513,469,501,487,374,519,421,601,455,425,464,496,449,
    443,436,552,554,388,555,301,498,466,445,420,659,617,538,665,388,684,584,536,189,
    536,602,369,492,533,437,755,563,606,425,434,440,564,514,445,646,392,406,631,383,
    596,402,621,543,566,496,659,336,411,547,705,537,456,416,459,421,590,471,473,538,
    375,439,297,440,520,508,496,421,519,415,535,649,560,474,422,533,603,491,572,392,
    336,409,627,478,358,577,655,572,367,654,354,384,622,395,703,535,654,508,520,327,
    474,323,373,501,413,590,601,647,576,500,481,642,455,462,529,435,539,513,466,633,
    460,503,617,429,435,552,517,383,555,540,547,600,336,375,674,555,618,571,465,356,
    582,474,497,623,468,383,561,546,375,670,484,494,413,428,381,535,511,409,511,350,
    514,672,413,648,338,564,487,437,524,627,442,562,446,585,660,582,550,373,525,620,
    464,365,495,566,573,557,564,516,545,573,508,361,634,648,609,574,434,534,363,458,
    495,746,387,457,570,393,451,425,541,458,599,522,479,436,501,464,544,471,510,542,
    529,463,369,603,563,410,371,508,634,435,519,419,498,470,529,528,565,470,432,501,
    542,453,507,522,521,590,454,495,284,430,393,500,261,446,435,313,475,530,482,607,
    531,447,521,582,699,397,583,447,389,364,624,470,441,428,458,632,475,373,453,463,
    280,680,290,570,689,396,527,229,570,479,600,519,478,482,563,520,510,489,551,629,
    503,351,491,323,377,600,544,557,615,571,570,467,384,508,422,523,706,503,715,457,
    319,429,592,506,489,577,508,409,596,559,629,617,399,615,403,540,462,447,590,441,
    648,626,498,448,349,432,502,419,697,605,429,523,405,438,237,443,453,430,656,459,
    504,521,571,488,419,379,494,501,472,436,436,468,457,450,478,271,388,371,499,470,
    484,621,600,526,590,406,676,502,457,456,554,444,491,415,593,564,411,426,669,628,
    615,449,610,453,490,335,453,399,723,469,523,468,470,495,609,542,495,515,599,549,
    477,402,569,511,495,345,539,470,462,463,231,528,489,663,602,349,468,455,513,368,
    423,438,456,459,402,423,398,489,568,496,638,563,595,517,560,601,522,470,476,394,
    486,510,294,478,512,443,373,569,509,400,589,571,505,589,773,579,405,382,475,420,
    510,734,588,546,490,560,441,477,381,587,421,366,451,410,434,670,575,560,548,566,
    508,571,733,528,607,597,474,521,543,615,398,534,479,503,452,601,396,460,502,438,
    586,424,395,574,519,393,569,605,533,494,560,498,486,496,662,577,532,514,355,309,
    569,445,641,702,659,539,519,555,473,472,483,525,609,405,350,358,450,410,413,458,
    392,467,338,455,482,383,513,344,475,608,492,510,479,402,375,510,490,524,605,465,
    562,443,627,676,602,608,615,560,466,477,445,396,726,405,398,442,528,391,369,503,
    474,587,424,545,424,514,538,499,350,396,511,517,363,522,493,652,532,581,571,455,
    443,531,518,497,767,466,631,276,495,407,573,472,590,529,500,560,715,513,531,389,
    270,393,268,468,496,529,504,535,526,585,449,534,369,365,350,474,477,500,580,557,
    457,558,644,497,616,445,392,437,657,635,533,666,682,483,575,593,533,507,308,434,
    639,516,458,546,562,696,479,485,640,465,566,521,590,511,353,540,513,620,630,450,
    443,553,439,522,480,514,451,511,536,437,611,530,618,492,496,609,607,607,482,534,
    532,490,309,332,732,523,427,434,425,521,442,409,722,596,226,435,501,446,465,625,
    440,374,416,468,441,555,548,492,750,631,498,469,524,491,515,386,475,574,757,444,
    599,483,431,620,387,444,484,446,434,498,532,367,739,329,488,524,465,546,522,345,
    488,472,730,567,489,541,517,424,419,299,547,434,513,474,406,404,540,476,360,288,
    623,338,520,444,524,619,325,503,557,696,562,584,426,471,454,568,408,356,749,546,
    582,369,349,571,439,470,306,445,506,417,522,405,594,478,409,476,528,568,389,605,
    571,480,516,555,375,523,492,536,415,490,508,585,569,561,665,536,445,457,582,542,
    530,375,571,568,635,534,572,428,548,563,530,457,543,426,480,473,457,402,430,573,
    569,458,719,405,449,332,426,631,437,375,420,641,490,411,515,627,360,514,686,567,
    661,506,409,675,414,461,408,492,507,435,481,428,641,638,516,420,390,474,602,469,
    397,557,524,594,456,570,670,576,490,481,366,512,650,569,481,543,291,487,338,356,
    425,424,579,392,346,391,379,608,576,640,485,625,391,468,437,489,326,630,550,748,
    549,411,575,508,589,471,497,490,691,471,446,568,333,534,596,370,550,296,438,439,
    602,645,599,446,389,579,451,457,466,453,478,492,624,512,398,432,414,485,485,389,
    561,306,523,435,617,545,559,345,392,417,491,518,496,405,529,594,618,584,375,614,
    459,513,578,308,527,580,338,571,474,537,544,625,284,368,540,319,429,608,454,382,
    560,464,459,533,524,531,549,549,555,596,580,574,455,408,526,557,218,491,399,516,
    437,357,459,497,278,484,416,546,419,577,374,580,431,579,438,517,389,619,360,442,
    677,550,538,301,489,384,797,542,587,580,473,615,441,469,449,403,643,525,507,628,
    386,275,354,523,347,590,479,601,634,579,496,635,533,544,569,451,375,512,488,345,
    564,313,503,451,476,597,466,422,546,526,529,679,552,345,548,507,640,294,508,506,
    415,615,517,647,489,492,567,589,637,448,616,226,455,505,627,549,522,529,596,658,
    617,426,440,659,374,512,485,541,593,436,459,513,524,654,424,623,608,421,466,260,
    490,454,690,396,454,554,615,444,523,671,394,662,555,607,523,750,575,417,608,595,
    489,448,668,776,490,410,490,418,594,479,623,322,626,398,343,649,701,590,523,490,
    482,581,393,476,510,472,516,461,457,440,463,643,559,531,479,556,405,498,384,706,
    520,669,491,619,563,556,395,784,466,509,440,575,497,438,482,608,503,314,444,549,
    493,643,341,590,482,488,675,504,601,430,379,490,584,482,537,441,400,638,613,489,
    584,355,445,345,569,555,617,427,549,461,511,585,399,643,556,340,530,544,561,504,
    229,436,515,452,272,562,420,588,620,325,563,645,513,505,634,509,543,614,628,385,
    511,500,218,704,548,550,489,513,483,515,823,575,481,508,596,609,366,609,476,500,
    645,617,584,486,516,504,379,666,473,584,501,511,368,486,421,416,662,318,516,366,
    563,431,562,503,498,534,310,390,582,470,527,543,494,637,578,618,498,518,521,289,
    426,599,552,513,543,480,638,536,404,495,433,324,406,573,507,536,459,493,546,571,
    393,478,419,662,492,598,424,312,569,607,594,597,533,328,571,399,326,615,472,513,
    541,262,490,375,569,568,617,544,555,615,642,314,511,430,455,453,567,502,714,434,
    477,522,507,393,490,446,323,494,454,659,504,601,533,458,564,242,546,510,271,418,
    431,699,423,251,451,496,519,606,372,586,457,629,516,434,629,522,523,501,538,348,
    479,424,464,457,502,571,506,545,476,453,421,542,588,499,440,434,590,515,429,489,
    372,574,587,594,543,373,504,401,277,516,481,574,480,439,208,526,292,468,525,535,
    305,448,493,466,582,437,537,470,459,495,591,408,715,494,488,598,557,519,566,432,
    641,527,483,420,539,530,393,439,369,454,599,606,516,473,561,456,617,541,462,515,
    471,518,555,579,626,399,518,665,534,338,358,506,443,520,446,417,464,475,556,429,
    229,452,603,557,616,507,495,548,443,612,662,573,519,244,571,450,644,606,354,670,
    589,429,661,608,422,185,558,503,754,486,466,579,502,444,481,621,643,503,630,549,
    459,620,609,618,355,392,595,546,443,508,405,376,458,442,564,472,237,646,445,460,
    535,373,483,507,582,380,478,427,551,551,574,383,451,583,613,406,513,348,340,484,
    397,642,370,402,454,524,572,589,744,466,531,378,502,526,420,682,535,428,623,439,
    423,343,476,437,657,503,565,366,402,427,462,367,445,592,498,542,443,483,729,345,
    516,397,526,626,498,451,334,514,514,490,429,537,473,369,684,560,694,516,532,437,
    594,498,491,614,623,514,369,547,715,454,486,358,466,472,571,463,487,639,595,605,
    433,324,598,441,320,522,678,442,481,590,492,392,466,572,369,473,477,412,505,547,
    502,395,526,357,492,393,482,543,507,399,466,494,482,629,647,460,588,457,498,395,
    215,455,447,657,639,683,317,467,568,555,431,456,414,617,415,456,449,351,446,581,
    775,640,400,412,505,555,331,688,463,572,745,482,453,567,589,545,505,665,471,502,
    315,494,472,557,633,605,458,427,585,469,659,549,485,671,448,575,370,570,424,555,
    600,437,461,489,518,512,354,378,690,328,504,604,611,451,419,436,340,470,559,531};

uint32_t uniformDistribution[] = {
    981,298,862,446,79,7,997,245,951,81,145,869,49,625,806,246,858,958,910,166,
    85,632,533,828,868,324,157,297,299,884,16,970,872,878,106,951,885,102,196,526,
    874,31,394,923,346,890,169,204,847,769,370,933,400,903,760,958,227,607,255,216,
    180,271,877,51,148,983,2,724,775,889,249,648,610,333,261,957,222,430,160,68,
    198,530,691,289,123,140,937,40,748,191,256,928,152,132,670,992,805,672,405,579,
    250,654,918,860,677,178,816,589,298,976,347,186,196,38,165,319,178,102,359,616,
    293,305,544,446,128,213,127,623,575,532,893,825,876,810,684,552,988,190,140,976,
    165,177,161,51,215,327,60,84,429,109,700,412,415,934,548,233,146,675,856,411,
    206,748,235,81,247,610,323,925,800,153,900,655,331,61,707,236,78,457,320,197,
    567,711,609,672,644,157,905,481,522,450,892,729,198,818,500,445,117,514,60,607,
    667,960,261,688,711,658,925,479,115,935,676,372,645,285,43,289,132,948,460,654,
    87,351,72,285,859,263,421,976,777,481,272,133,130,534,512,842,882,436,10,687,
    370,687,58,706,972,101,685,794,739,144,137,827,185,901,802,44,854,913,710,320,
    83,983,453,214,206,965,746,87,90,756,775,151,442,523,857,103,625,541,587,53,
    685,725,570,870,315,372,604,168,975,4,488,58,987,631,272,883,286,708,970,376,
    463,434,217,596,648,73,389,272,304,977,15,679,391,586,239,706,648,843,564,622,
    537,51,370,213,372,642,95,658,39,756,725,503,880,942,789,527,15,177,489,9,
    844,505,689,925,781,618,320,118,460,884,430,688,625,801,901,998,132,997,655,172,
    752,69,675,321,11,153,849,717,330,337,726,865,532,104,789,312,722,109,430,873,
    683,551,560,308,351,460,305,483,146,650,345,588,720,710,910,421,863,758,137,884,
    785,863,748,317,968,226,629,689,335,750,251,18,300,811,16,341,271,11,514,107,
    351,860,696,761,569,605,181,122,52,318,5,837,872,443,153,529,669,473,217,695,
    222,469,403,212,970,419,243,931,430,757,38,471,306,424,232,566,28,413,688,771,
    732,383,607,293,826,451,822,184,924,729,879,836,888,972,738,858,390,981,788,510,
    427,516,982,734,940,213,990,658,316,677,428,738,59,726,30,575,867,852,759,790,
    581,638,625,468,609,362,15,690,32,494,890,459,9,561,883,640,464,872,297,781,
    548,416,518,297,141,239,872,7,90,631,487,361,959,802,520,257,854,535,637,886,
    28,527,34,729,87,918,368,552,789,665,22,27,771,540,14,912,469,887,609,560,
    207,786,611,856,587,130,803,440,356,440,15,74,967,50,803,744,968,861,986,446,
    216,7,473,987,238,178,589,707,755,888,957,652,674,568,507,951,388,309,81,434,
    439,96,509,95,837,1,840,804,553,825,941,769,523,103,755,761,281,343,467,35,
    922,114,687,595,682,884,545,760,193,626,194,322,413,393,418,249,84,948,744,637,
    462,684,95,985,787,851,745,68,884,903,794,495,707,481,89,78,364,325,838,247,
    641,722,570,53,805,988,303,890,935,46,216,86,420,312,71,206,853,506,274,426,
    98,759,922,805,239,701,883,603,25,411,541,667,132,110,720,628,788,713,517,412,
    449,423,498,869,425,259,75,277,766,39,394,864,798,5,359,727,706,241,20,732,
    652,561,88,475,361,498,102,839,211,309,941,350,732,128,219,848,388,985,124,153,
    714,208,707,512,213,65,929,920,998,949,341,339,199,429,504,560,617,606,88,828,
    605,28,869,27,157,87,875,545,71,689,388,475,898,785,677,110,541,606,720,538,
    554,751,567,754,179,71,3,487,367,92,314,663,811,182,690,968,960,564,202,721,
    943,280,196,840,65,873,641,606,168,50,834,723,802,90,166,671,161,860,157,219,
    642,162,882,453,34,261,110,995,515,313,405,457,283,601,988,38,164,318,334,332,
    368,167,745,169,949,601,531,800,151,378,18,793,540,590,936,575,851,46,259,365,
    49,664,513,332,956,500,371,810,818,705,832,876,563,577,736,511,868,266,310,18,
    644,19,502,875,299,437,139,150,483,398,205,222,752,408,245,397,598,616,206,415,
    10,729,982,263,996,717,774,863,673,775,572,6,484,73,881,783,200,710,623,374,
    107,519,596,550,927,841,637,525,146,534,630,848,262,611,110,257,17,575,119,690,
    349,381,387,523,144,958,996,345,668,619,409,465,137,4,14,63,536,342,278,372,
    876,909,219,137,209,330,394,918,905,203,297,944,275,684,156,419,642,151,454,309,
    770,863,464,597,558,169,661,784,511,629,155,386,227,375,523,127,395,607,44,990,
    500,341,933,775,25,88,194,357,239,338,356,700,201,820,296,449,989,647,232,499,
    276,387,885,193,452,97,320,537,394,364,526,895,706,458,669,421,236,553,778,166,
    892,133,866,783,643,852,231,632,499,153,130,465,230,706,658,373,493,979,910,888,
    342,436,782,738,584,140,158,511,384,626,677,966,449,232,748,92,774,669,724,272,
    512,544,427,742,940,85,114,433,63,715,10,95,150,482,524,424,312,682,935,696,
    999,301,661,447,223,98,539,998,457,953,960,969,497,387,401,126,472,205,249,225,
    920,259,10,760,431,534,184,744,907,809,439,905,801,791,351,23,889,581,20,346,
    533,981,4,720,367,405,537,529,611,786,754,220,736,764,981,166,298,855,600,204,
    663,39,799,153,520,149,177,408,420,888,444,954,868,139,363,925,234,900,453,535,
    376,897,756,111,661,426,968,649,280,568,853,633,607,651,787,817,490,654,224,911,
    541,359,554,409,188,607,23,422,197,477,958,573,373,713,684,724,138,651,372,108,
    218,915,431,515,565,217,331,746,561,246,346,102,295,900,201,483,196,224,905,393,
    391,552,966,455,955,339,178,92,681,241,891,589,155,322,104,411,229,125,156,791,
    61,192,893,356,782,93,529,978,7,124,61,399,676,26,544,631,56,412,413,737,
    653,995,325,809,6,119,910,235,245,65,716,997,257,298,43,729,81,572,706,780,
    386,767,178,753,484,412,383,540,824,486,967,477,170,982,976,176,101,885,102,36,
    640,508,32,587,807,75,315,578,338,20,357,724,478,225,476,962,637,549,191,461,
    726,848,628,896,829,603,763,930,487,865,966,817,372,689,403,869,454,718,447,792,
    428,804,206,906,720,372,557,356,922,438,507,337,285,134,923,114,737,685,734,914,
    549,390,730,612,78,132,480,532,540,617,14,969,111,220,564,831,592,812,877,203,
    249,384,230,225,518,153,339,255,838,72,168,77,462,589,379,230,721,859,453,261,
    166,467,920,277,377,483,798,659,294,674,553,234,748,783,459,266,936,488,211,464,
    250,69,541,403,658,920,633,69,468,776,330,324,242,249,601,309,422,88,969,407,
    453,521,331,200,303,790,156,930,277,57,393,217,127,624,620,475,543,253,544,701,
    28,564,25,962,813,316,270,926,405,929,332,548,449,663,438,443,142,595,372,109,
    652,455,326,469,78,947,945,311,890,178,11,608,743,36,569,245,43,530,170,138,
    458,192,686,598,545,123,731,687,408,793,486,751,247,813,219,325,449,854,636,338,
    723,337,636,465,64,205,400,107,425,261,245,883,453,621,170,999,434,901,375,843,
    383,552,283,630,54,192,645,503,46,971,531,459,308,166,614,372,61,13,169,486,
    274,104,59,418,725,920,106,158,821,481,691,894,723,974,524,777,166,168,279,903,
    830,500,51,137,667,665,199,418,678,368,905,643,162,964,60,887,883,166,735,393,
    337,426,287,60,89,501,837,946,359,807,848,188,306,899,15,663,253,214,81,932,
    272,986,574,434,639,324,10,211,490,746,295,827,862,582,577,641,773,414,587,131,
    220,124,10,216,714,25,570,967,931,651,588,202,326,852,327,655,175,337,866,665,
    773,160,182,324,432,759,966,895,863,552,26,773,366,727,990,79,752,559,737,372,
    900,324,575,225,176,592,880,351,619,435,707,82,596,579,406,718,27,371,613,891,
    613,329,663,980,55,342,749,497,591,485,870,490,810,134,405,676,416,975,717,34,
    410,113,116,696,692,523,103,720,584,716,300,888,735,963,557,480,305,305,978,586,
    791,847,76,290,671,171,966,86,146,372,811,246,486,928,632,177,140,735,587,414,
    141,887,301,876,540,858,356,535,854,23,120,334,560,887,624,230,58,279,6,895,
    651,817,831,827,434,462,695,574,887,281,679,27,859,980,594,398,528,950,933,381,
    973,743,715,532,630,28,452,378,307,458,963,648,965,793,475,399,945,860,973,831,
    140,341,549,689,11,142,777,539,782,399,610,444,142,324,666,462,352,117,530,349,
    265,492,687,229,975,852,628,609,711,291,439,542,632,678,230,643,820,698,872,601,
    96,481,44,929,805,710,80,847,517,610,886,782,101,573,702,766,424,329,374,826,
    310,504,367,943,181,287,275,692,985,147,983,772,318,718,390,123,427,471,660,635,
    80,546,106,873,809,808,638,923,137,12,748,137,206,805,79,387,783,45,769,457,
    883,752,228,200,469,619,13,586,780,364,220,860,600,327,732,408,825,370,20,652,
    72,459,790,278,263,559,355,45,604,815,503,486,566,421,377,725,730,390,310,509,
    754,221,369,353,238,791,451,62,160,472,715,923,931,504,200,884,62,246,620,357
};
/* clang-format on */
class PacketJitterEmulator
{
public:
    PacketJitterEmulator(memory::PacketPoolAllocator& allocator, uint32_t ssrc)
        : _source(allocator, 80, ssrc),
          _jitter(10),
          _link("Jitter", 2500, 15000, 1500)
    {
    }

    void setJitter(float ms) { _jitter = ms; }

    memory::UniquePacket get(uint64_t timestamp)
    {
        auto packet = _source.getPacket(timestamp);
        if (packet)
        {
            _link.push(std::move(packet), timestamp, false);
            if (_link.count() == 1)
            {
                uint32_t latency = (rand() % static_cast<uint32_t>(_jitter * 101)) / 100;
                _link.injectDelaySpike(latency);
            }
        }

        return _link.pop(timestamp);
    }

    uint64_t nextEmitTime(uint64_t timestamp) const { return _link.timeToRelease(timestamp); }

private:
    fakenet::FakeAudioSource _source;
    double _jitter;

    memory::UniquePacket _packet;
    fakenet::NetworkLink _link;
};
} // namespace

TEST(Welford, uniform)
{
    math::WelfordVariance<double> w1;
    math::RollingWelfordVariance<double> w2(250);
    uint32_t range = 1000;
    for (int i = 0; i < 2000; ++i)
    {
        double val = uniformDistribution[i];

        w1.add(val);
        w2.add(val);

        if (i % 100 == 0)
        {
            logger::debug("w1 %f, %f, w2 %f, %f",
                "",
                w1.getMean(),
                sqrt(w1.getVariance()),
                w2.getMean(),
                sqrt(w2.getVariance()));
        }
    }

    EXPECT_NEAR(w2.getMean(), 500.0, 22.0);
    EXPECT_NEAR(w2.getVariance(), (range * range) / 12, 6000);
}

TEST(Welford, normal)
{
    math::WelfordVariance<double> w1;
    math::RollingWelfordVariance<double> w2(250);

    std::random_device rd{};
    std::mt19937 gen{rd()};

    // values near the mean are the most likely
    // standard deviation affects the dispersion of generated values from the mean
    for (int i = 0; i < 2500; ++i)
    {
        double val = normalDistribution[i];
        w1.add(val);
        w2.add(val);

        if (i % 100 == 0)
        {
            logger::debug("w1 %f, %f, w2 %f, %f",
                "",
                w1.getMean(),
                sqrt(w1.getVariance()),
                w2.getMean(),
                sqrt(w2.getVariance()));
        }
    }
    EXPECT_NEAR(w2.getMean(), 500.0, 20.0);
    EXPECT_NEAR(w2.getVariance(), 100.0 * 100, 2500.0);
}

class JitterBufferTest : public ::testing::Test
{
public:
    JitterBufferTest() : _allocator(400, "test") {}

    memory::PacketPoolAllocator _allocator;
    rtp::JitterBufferList _buffer;
};

// jitter below 10ms will never affect subsequent packet. It is just a delay inflicted on each packet unrelated to
// previous events
TEST(JitterTest, a10ms)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    rtp::JitterEstimator jitt(48000);
    emulator.setJitter(10);

    uint64_t timestamp = utils::Time::getAbsoluteTime();

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 600);
    EXPECT_GE(countBelow, 600);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 5.5, 1.5);
}

// delay above 20ms will affect subsequent packet as previous packet has not been delivered yet.
TEST(JitterTest, a30ms)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    emulator.setJitter(30);
    rtp::JitterEstimator jitt(48000);
    rtp::Backlog<double, 1500> blog;
    math::RollingWelfordVariance<double> var2(250);
    uint64_t timestamp = utils::Time::getAbsoluteTime();

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());
            blog.add(delay);
            var2.add(delay);

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
    }

    auto m = jitt.getJitter();
    for (int i = 0; i < 10; ++i)
    {
        auto a = m - 2.0 + i * 0.4;
        logger::info("m %.4f  std %.4f", "", a, blog.getVariance(a));
    }
    logger::info("roll wf m %.4f  std %.4f", "", var2.getMean(), blog.getVariance(var2.getMean()));

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 600);
    EXPECT_GE(countBelow, 600);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 12.0, 1.5);
}

TEST(JitterTest, clockSkewRxFaster)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();
    uint64_t remoteTimestamp = timestamp;

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(remoteTimestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
        remoteTimestamp += utils::Time::ms * 2;
        // 500 iterations per s
        if (i % 5 == 0) // 0.01s
        {
            remoteTimestamp += utils::Time::us * 5; // 0.5ms / s
        }
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 650);
    EXPECT_GE(countBelow, 650);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 5.5, 1.5);
}

TEST(JitterTest, clockSkewRxSlower)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();
    uint64_t remoteTimestamp = timestamp;

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(remoteTimestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
        remoteTimestamp += utils::Time::ms * 2;
        if (i % 5 == 0) // 0.01s
        {
            timestamp += utils::Time::us * 5;
        }
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 690);
    EXPECT_GE(countBelow, 690);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 5.5, 1.5);
}

TEST(JitterTest, a230)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    emulator.setJitter(230);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 600);
    EXPECT_GE(countBelow, 600);
    EXPECT_GE(below95, 1400);
    EXPECT_NEAR(jitt.getJitter(), 75, 15.5);
}

TEST(JitterTest, gap)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        auto p = emulator.get(timestamp);
        if (p && (i < 300 || i > 450))
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 600);
    EXPECT_GE(countBelow, 600);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 5.5, 1.5);
}

TEST(JitterTest, dtx)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    rtp::RtpDelayTracker delayTracker(48000);

    uint64_t receiveTime = 5000;
    uint32_t rtpTimestamp = 7800;
    for (int i = 0; i < 300; ++i)
    {
        delayTracker.update(receiveTime + (rand() % 40) * utils::Time::ms / 10, rtpTimestamp);
        receiveTime += utils::Time::ms * 20;
        rtpTimestamp += 960;
    }

    receiveTime += utils::Time::minute * 3;
    rtpTimestamp += 24 * 60 * 48000; // 24 min increase in rtptimestamp
    auto delay = delayTracker.update(receiveTime, rtpTimestamp);
    EXPECT_EQ(delay, 0);

    rtpTimestamp += 960;
    receiveTime += utils::Time::ms * 20;
    delay = delayTracker.update(receiveTime + 2 * utils::Time::ms, rtpTimestamp);
    EXPECT_NEAR(delay, 2.0 * utils::Time::ms, 11 * utils::Time::us);

    rtpTimestamp -= 3 * 48000; // rollback rtp by 3s, would look like huge 3s jitter
    receiveTime += utils::Time::ms * 20;
    delay = delayTracker.update(receiveTime, rtpTimestamp);
    EXPECT_LE(delay, utils::Time::ms * 10);

    rtpTimestamp += 960;
    receiveTime += utils::Time::ms * 200;
    delay = delayTracker.update(receiveTime, rtpTimestamp);
    EXPECT_GE(delay, utils::Time::ms * 179);

    // run for 10s
    for (int i = 0; i < 9 * 50; ++i)
    {
        rtpTimestamp += 960;
        receiveTime += utils::Time::ms * 20;
        delay = delayTracker.update(receiveTime + (rand() % 4) * utils::Time::ms, rtpTimestamp);
    }
    EXPECT_LT(delay, utils::Time::ms * 9);

    receiveTime += utils::Time::sec * 10;
    for (int i = 0; i < 10 * 50; ++i)
    {
        rtpTimestamp += 960;
        receiveTime += utils::Time::ms * 20;
        delay = delayTracker.update(receiveTime + (rand() % 4) * utils::Time::ms, rtpTimestamp);
    }
    EXPECT_LT(delay, utils::Time::ms * 100);
}

TEST(JitterTest, adaptDown23)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    emulator.setJitter(200);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 15000; ++i)
    {
        if (i == 9980)
        {
            EXPECT_NEAR(jitt.getJitter(), 72, 14.0);
            emulator.setJitter(23);
            logger::info("decrease jitter to 23ms", "");
        }

        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 500);
    EXPECT_GE(countBelow, 500);
    EXPECT_GE(below95, 1440);
    EXPECT_NEAR(jitt.getJitter(), 10, 1.5);
}

TEST(JitterTest, adaptUp200)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);

    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();
    rtp::JitterTracker tracker(48000);

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 55000; ++i)
    {
        if (i == 9980)
        {
            EXPECT_NEAR(jitt.getJitter(), 4.5, 1.5);
            emulator.setJitter(200);
            logger::info("incr jitter to 30ms", "");
        }

        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());
            tracker.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
        if (i % 500 == 0)
        {
            logger::info("jitter %f, 95p %f, maxJ %f, ietf jitter %fms",
                "JitterEstimator",
                jitt.getJitter(),
                jitt.get95Percentile(),
                jitt.getMaxJitter(),
                tracker.get() / 48.0);
        }
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 600);
    EXPECT_GE(countBelow, 600);
    EXPECT_GE(below95, 1430);
    EXPECT_NEAR(jitt.getJitter(), 75, 13.5);
}

TEST(JitterTest, adaptUp30)
{
    memory::PacketPoolAllocator allocator(3000, "TestAllocator");
    PacketJitterEmulator emulator(allocator, 5670);
    emulator.setJitter(10);
    rtp::JitterEstimator jitt(48000);

    uint64_t timestamp = utils::Time::getAbsoluteTime();
    rtp::JitterTracker tracker(48000);

    int countBelow = 0;
    int countAbove = 0;
    int below95 = 0;
    for (int i = 0; i < 12500; ++i)
    {
        if (i == 9980)
        {
            EXPECT_NEAR(jitt.getJitter(), 4.5, 1.5);
            emulator.setJitter(30);
            logger::info("incr jitter to 30ms", "");
        }

        auto p = emulator.get(timestamp);
        if (p)
        {
            auto header = rtp::RtpHeader::fromPacket(*p);
            auto delay = jitt.update(timestamp, header->timestamp.get());
            tracker.update(timestamp, header->timestamp.get());

            if (delay > jitt.getJitter())
            {
                ++countAbove;
            }
            else
            {
                ++countBelow;
            }

            if (delay < jitt.get95Percentile())
            {
                ++below95;
            }
        }
        timestamp += utils::Time::ms * 2;
        if (i % 500 == 0)
        {
            logger::info("jitter %f, 95p %f, maxJ %f, ietf jitter %fms",
                "JitterEstimator",
                jitt.getJitter(),
                jitt.get95Percentile(),
                jitt.getMaxJitter(),
                tracker.get() / 48.0);
        }
    }

    logger::info("jitter %f, 95p %f, maxJ %f",
        "JitterEstimator",
        jitt.getJitter(),
        jitt.get95Percentile(),
        jitt.getMaxJitter());

    logger::info("above %d, below %d, 95p %d", "", countAbove, countBelow, below95);
    EXPECT_GE(countAbove, 580);
    EXPECT_GE(countBelow, 580);
    EXPECT_GE(below95, 1150);
    EXPECT_NEAR(jitt.getJitter(), 12, 2.5);
}

TEST(JitterTest, jitterTracker)
{
    rtp::JitterTracker tracker(48000);

    uint32_t rtpTimestamp = 500;
    uint64_t receiveTime = utils::Time::getAbsoluteTime();

    for (int i = 0; i < 8000; ++i)
    {
        tracker.update(receiveTime, rtpTimestamp);
        rtpTimestamp += 960; // 20ms
        receiveTime += (15 + rand() % 11) * utils::Time::ms;
    }

    logger::debug("jit %u", "", tracker.get());
    EXPECT_NEAR(tracker.get(), 25 * 48 / 10, 5);

    // receive with 2ms difference in interval compared to send interval
    for (int i = 0; i < 8000; ++i)
    {
        tracker.update(receiveTime, rtpTimestamp);
        rtpTimestamp += 960; // 20ms
        receiveTime += i & 1 ? 22 * utils::Time::ms : 18 * utils::Time::ms;
    }
    logger::debug("jit %u", "", tracker.get());
    EXPECT_NEAR(tracker.get(), 2 * 48, 15);
}

TEST_F(JitterBufferTest, bufferPlain)
{
    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }
    const auto oneFromFull = _buffer.SIZE - 2;

    for (int i = 0; i < oneFromFull; ++i)
    {
        auto p = memory::makeUniquePacket(_allocator, stageArea);
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = i + 100;
        header->timestamp = 56000 + i * 960;

        EXPECT_TRUE(_buffer.add(std::move(p)));
    }

    EXPECT_EQ(_buffer.count(), oneFromFull);
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * (oneFromFull - 1));
    EXPECT_FALSE(_buffer.empty());
    EXPECT_EQ(_buffer.getFrontRtp()->timestamp.get(), 56000);

    // make a gap of 3 pkts
    auto p = memory::makeUniquePacket(_allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 100 + oneFromFull;
        header->timestamp = 56000 + oneFromFull * 960;
    }
    EXPECT_TRUE(_buffer.add(std::move(p)));
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * oneFromFull);

    uint16_t prevSeq = 99;
    uint32_t count = 0;
    EXPECT_EQ(_buffer.count(), oneFromFull + 1);
    for (auto packet = _buffer.pop(); packet; packet = _buffer.pop())
    {
        auto header = rtp::RtpHeader::fromPacket(*packet);
        EXPECT_GT(header->sequenceNumber.get(), prevSeq);
        prevSeq = header->sequenceNumber.get();
        ++count;
    }
    EXPECT_EQ(count, oneFromFull + 1);
}

TEST_F(JitterBufferTest, bufferReorder)
{
    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }

    for (int i = 0; i < 5; ++i)
    {
        auto p = memory::makeUniquePacket(_allocator, stageArea);
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = i + 100;
        header->timestamp = 56000 + i * 960;

        EXPECT_TRUE(_buffer.add(std::move(p)));
    }

    EXPECT_EQ(_buffer.count(), 5);
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * 4);
    EXPECT_FALSE(_buffer.empty());
    EXPECT_EQ(_buffer.getFrontRtp()->timestamp.get(), 56000);

    // make a gap
    auto p = memory::makeUniquePacket(_allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 110;
        header->timestamp = 56000 + 10 * 960;
    }
    EXPECT_TRUE(_buffer.add(std::move(p)));
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * 10);

    // reorder
    p = memory::makeUniquePacket(_allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 105;
        header->timestamp = 56000 + 5 * 960;
    }
    EXPECT_TRUE(_buffer.add(std::move(p)));
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * 10);
    EXPECT_EQ(_buffer.count(), 7);

    p = memory::makeUniquePacket(_allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 108;
        header->timestamp = 56000 + 28 * 960;
    }
    EXPECT_TRUE(_buffer.add(std::move(p)));

    uint16_t prevSeq = 99;
    uint32_t count = 0;
    for (auto packet = _buffer.pop(); packet; packet = _buffer.pop())
    {
        auto header = rtp::RtpHeader::fromPacket(*packet);
        EXPECT_GT(header->sequenceNumber.get(), prevSeq);
        prevSeq = header->sequenceNumber.get();
        ++count;
    }
    EXPECT_EQ(count, 8);
}

TEST_F(JitterBufferTest, bufferEmptyFull)
{
    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }

    EXPECT_EQ(_buffer.pop(), nullptr);

    for (int i = 0; i < _buffer.SIZE; ++i)
    {
        auto p = memory::makeUniquePacket(_allocator, stageArea);
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = i + 100;
        header->timestamp = 56000 + i * 960;

        EXPECT_TRUE(_buffer.add(std::move(p)));
    }

    EXPECT_EQ(_buffer.count(), _buffer.SIZE);
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * (_buffer.SIZE - 1));
    EXPECT_FALSE(_buffer.empty());
    EXPECT_EQ(_buffer.getFrontRtp()->timestamp.get(), 56000);

    auto p = memory::makeUniquePacket(_allocator, stageArea);
    {
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = 100 + _buffer.count();
        header->timestamp = 56000 + 10 * 960;
    }
    EXPECT_FALSE(_buffer.add(std::move(p)));
    EXPECT_EQ(_buffer.getRtpDelay(), 960 * (_buffer.SIZE - 1));

    uint16_t prevSeq = 99;
    uint32_t count = 0;
    EXPECT_EQ(_buffer.count(), _buffer.SIZE);
    for (auto packet = _buffer.pop(); packet; packet = _buffer.pop())
    {
        auto header = rtp::RtpHeader::fromPacket(*packet);
        EXPECT_GT(header->sequenceNumber.get(), prevSeq);
        prevSeq = header->sequenceNumber.get();
        ++count;
    }
    EXPECT_EQ(count, _buffer.SIZE);
    EXPECT_TRUE(_buffer.empty());
    EXPECT_EQ(_buffer.pop(), nullptr);
}

TEST_F(JitterBufferTest, reorderedFull)
{
    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }

    EXPECT_EQ(_buffer.pop(), nullptr);

    for (int i = 0; i < _buffer.SIZE - 50; ++i)
    {
        auto p = memory::makeUniquePacket(_allocator, stageArea);
        auto header = rtp::RtpHeader::create(*p);
        header->sequenceNumber = i + 100;
        header->timestamp = 56000 + i * 960;

        EXPECT_TRUE(_buffer.add(std::move(p)));
    }

    auto p = memory::makeUniquePacket(_allocator, stageArea);
    auto header = rtp::RtpHeader::create(*p);
    header->sequenceNumber = 49;
    header->timestamp = 56100;

    EXPECT_EQ(_buffer.count(), _buffer.SIZE - 50);
    EXPECT_TRUE(_buffer.add(std::move(p)));
}

TEST_F(JitterBufferTest, reorderedOne)
{
    memory::Packet stageArea;
    {
        auto header = rtp::RtpHeader::create(stageArea);
        header->ssrc = 4000;
        stageArea.setLength(250);
    }

    EXPECT_EQ(_buffer.pop(), nullptr);

    auto p = memory::makeUniquePacket(_allocator, stageArea);
    auto header = rtp::RtpHeader::create(*p);
    header->sequenceNumber = 100;
    header->timestamp = 56000;
    EXPECT_TRUE(_buffer.add(std::move(p)));

    p = memory::makeUniquePacket(_allocator, stageArea);
    header = rtp::RtpHeader::create(*p);
    header->sequenceNumber = 98;
    header->timestamp = 57000;
    EXPECT_TRUE(_buffer.add(std::move(p)));

    p = _buffer.pop();
    header = rtp::RtpHeader::fromPacket(*p);
    EXPECT_EQ(header->sequenceNumber.get(), 98);

    p = _buffer.pop();
    header = rtp::RtpHeader::fromPacket(*p);
    EXPECT_EQ(header->sequenceNumber.get(), 100);
}
