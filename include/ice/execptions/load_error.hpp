#ifndef ICE_LOAD_ERROR_HPP
#define ICE_LOAD_ERROR_HPP

#include <stdexcept>
namespace ice {
class load_error : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
}  // namespace ice

#endif  // ICE_LOAD_ERROR_HPP
