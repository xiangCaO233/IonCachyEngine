#ifndef ICE_BUFFER_ERROR_HPP
#define ICE_BUFFER_ERROR_HPP

#include <stdexcept>
namespace ice {
class buffer_error : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
}  // namespace ice

#endif  // ICE_BUFFER_ERROR_HPP
