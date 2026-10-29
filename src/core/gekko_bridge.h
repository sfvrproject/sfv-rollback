#pragma once
// GekkoNetAdapter over an ITransport.
//
// GekkoNet's adapter callbacks carry no user pointer, so the transport a
// session talks through is selected with a scope around every call into that
// session. That also lets two sessions share a process in tests.

#include "core/transport.h"

#include <gekkonet.h>

namespace sfv {

GekkoNetAdapter* GekkoAdapter();

class GekkoTransportScope {
public:
    explicit GekkoTransportScope(ITransport* transport);
    ~GekkoTransportScope();
    GekkoTransportScope(const GekkoTransportScope&) = delete;
    GekkoTransportScope& operator=(const GekkoTransportScope&) = delete;

private:
    ITransport* previous_;
};

} // namespace sfv
