#pragma once

#include <trx/game/objects/types.h>
#include <trx/game/rooms.h>

// Reports whether a movable block stands on the square at pos, or has begun
// to move onto it, with the top of the block at or above the height pos
// gives. A block claims the square it enters from the moment its animation
// shifts it, which the floor data reports only once the move ends.
bool MovableBlock_TestSquareClaimed(XYZ_32 pos);

// Block or unblock a block's box overlap index.
void MovableBlock_UpdateBox(const ITEM *item, bool blocked);

// Move a block at rest to where another game left it, the floor it makes and
// the box it blocks going with it. Returns false, moving nothing, for an item
// that is no movable block or is on the move itself.
bool MovableBlock_Relocate(int16_t item_num, XYZ_32 pos, int16_t room_num);

// Turn a block to face the way another game turned it, as a player there
// takes hold of it, its look kept as it was. Does nothing to an item that is
// no movable block.
void MovableBlock_SetRotation(int16_t item_num, int16_t rot_y);

// Lock a stack that is about to drop.
void MovableBlock_LockStack(XYZ_32 drop_pos, int16_t room_num);
// Drop a stack of blocks.
void MovableBlock_DropStack(XYZ_32 drop_pos, int16_t room_num);

// Shift a stack of blocks up or down in the y direction.
void MovableBlock_ShiftStackY(
    int32_t stack_height, XYZ_32 old_pos, int32_t new_y, int16_t room_num,
    bool reposition);

// Shift a stack of blocks in the x or z direction..
void MovableBlock_SlideStack(
    int32_t stack_height, XYZ_32 old_sector, const ITEM *dest_item,
    bool reposition);
