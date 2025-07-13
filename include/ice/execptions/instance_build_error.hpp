#ifndef ICE_INSTANCE_BUILD_ERROR_HPP
#define ICE_INSTANCE_BUILD_ERROR_HPP

#include <stdexcept>

namespace ice {
class instance_build_error : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
}  // namespace ice

#endif  // ICE_INSTANCE_BUILD_ERROR_HPP
