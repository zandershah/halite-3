#pragma once

namespace hlt {

typedef int Halite;
typedef int PlayerId;
typedef int EntityId;

enum Task { EXPLORE = 0x01, RETURN = 0x02, HARD_RETURN = 0x04, BLOCK = 0x08 };

}  // namespace hlt
