#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorDiagnostics.h"

struct Setting
{
    bool value = false;
    bool value_or_default() const { return value; }
};
struct Settings
{
    Setting DlssNrSuperiorSkipMvUnits, DlssNrSuperiorSkipInterPass, DlssNrSuperiorSkipCarrier, DlssNrSuperiorSkipGuided,
        DlssNrSuperiorSkipFinalShaping, DlssNrSuperiorSkipCompressionLow, DlssNrSuperiorSkipWarpCache,
        DlssNrSuperiorFullGuided;
    struct Method
    {
        unsigned value = 2;
        unsigned value_or_default() const { return value; }
    };
    Method DlssNrSuperiorMotionMethod;
};
void require(bool v)
{
    if (!v)
        throw std::runtime_error("Superior stage bypass policy failed");
}
int main()
{
    try
    {
        namespace D = DlssNr::SuperiorScaling::Diagnostics;
        require(!D::LoadMotionMethod({}, {}));
        require(D::LoadMotionMethod({}, true) == 0u && D::LoadMotionMethod({}, false) == 1u);
        for (unsigned method = 0; method < 3; ++method)
            require(D::LoadMotionMethod(method, true) == method && D::LoadMotionMethod(method, false) == method);
        require(!D::LoadMotionMethod(UINT32_MAX, {}));
        require(D::LoadMotionMethod(UINT32_MAX, true) == 0u);
        for (unsigned bits = 0; bits < 256; ++bits)
            for (unsigned method = 0; method < 3; ++method)
            {
                Settings s;
                s.DlssNrSuperiorMotionMethod.value = method;
                Setting* fields[] { &s.DlssNrSuperiorSkipMvUnits,      &s.DlssNrSuperiorSkipInterPass,
                                    &s.DlssNrSuperiorSkipCarrier,      &s.DlssNrSuperiorSkipGuided,
                                    &s.DlssNrSuperiorSkipFinalShaping, &s.DlssNrSuperiorSkipCompressionLow,
                                    &s.DlssNrSuperiorSkipWarpCache,    &s.DlssNrSuperiorFullGuided };
                for (unsigned i = 0; i < 8; ++i)
                    fields[i]->value = (bits & (1u << i)) != 0;
                const auto mask = D::Mask(s);
                require(mask == (bits | (method == 0 ? 256u : method == 2 ? 512u : 0u)));
                require(D::MotionMethod(s) == method && D::MotionMethodFromMask(mask) == method);
                require(!D::PrivateDlss(false, mask));
                require(D::PrivateDlss(true, mask) == !(bits & 4u));
                const auto scales = D::Scales({ .65f, .58f }, mask);
                require(scales.high == .65f && scales.low == ((bits & 32u) ? .65f : .58f));
                const auto gains = D::FinalGains({ .8f, .66f }, mask);
                require(gains.high == ((bits & 16u) ? 1.f : .8f) && gains.low == ((bits & 16u) ? 1.f : .66f));
                require(D::CachedWarp(mask) == !(bits & (64u | 8u)));
                require(D::NativeCenter(mask) == !(bits & (128u | 8u)));
                require(D::ContinuousMotion(mask) == (method != 0u));
            }
        Settings invalid;
        invalid.DlssNrSuperiorMotionMethod.value = UINT32_MAX;
        require(D::MotionMethod(invalid) == 2u);
        std::cout << "Superior stage bypass policy PASS: all 768 combinations; carrier/DLSS dependency, independent "
                     "gains and warp modes\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
