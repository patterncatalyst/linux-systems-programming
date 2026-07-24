// chatterd.hpp — the book's recurring peer-to-peer chat daemon (ch21-27),
// reduced to what the capstone needs: serve local clients, and bridge two
// chatterd instances across hosts so a message sent on one node is delivered
// to clients on the other. See chatterd.cpp for the bridging design (mirrors
// go/chatterd.go's doc comment exactly).
#pragma once

#include <string>

#include "telemetry.hpp"

namespace chatterd {

int serve(const std::string& host, int port, const std::string& node, const std::string& peer,
          const std::string& peer_node, telemetry::Handle& tel);

int send(const std::string& host, int port, const std::string& nick, const std::string& text, int timeout_ms);

int listen(const std::string& host, int port, const std::string& nick, int timeout_ms);

} // namespace chatterd
