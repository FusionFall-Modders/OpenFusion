#include "Vendors.hpp"

#include "servers/CNShardServer.hpp"

#include "PlayerManager.hpp"
#include "Items.hpp"
#include "Rand.hpp"

// 7 days
#define VEHICLE_EXPIRY_DURATION 604800

using namespace Vendors;

std::map<int32_t, std::vector<VendorListing>> Vendors::VendorTables;

static void vendorBuy(CNSocket* sock, CNPacketData* data) {
    auto req = (sP_CL2FE_REQ_PC_VENDOR_ITEM_BUY*)data->buf;
    Player* plr = PlayerManager::getPlayer(sock);

    // prepare fail packet
    INITSTRUCT(sP_FE2CL_REP_PC_VENDOR_ITEM_BUY_FAIL, failResp);
    failResp.iErrorCode = 0;

    if (req->iVendorID != req->iNPC_ID || Vendors::VendorTables.find(req->iVendorID) == Vendors::VendorTables.end()) {
        std::cout << "[WARN] Vendor with ID " << req->iVendorID << " mismatched or not found (buy)" << std::endl;
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_ITEM_BUY_FAIL);
        return;
    }

    std::vector<VendorListing>* listings = &Vendors::VendorTables[req->iVendorID];
    VendorListing reqItem;
    reqItem.id = req->Item.iID;
    reqItem.type = req->Item.iType;
    reqItem.sort = 0; // just to be safe

    if (std::find(listings->begin(), listings->end(), reqItem) == listings->end()) { // item not found in listing
        std::cout << "[WARN] Player " << PlayerManager::getPlayerName(plr) << " tried to buy an item that wasn't on sale" << std::endl;
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_ITEM_BUY_FAIL);
        return;
    }

    Items::Item* itemDat = Items::getItemData(req->Item.iID, req->Item.iType);
    if (itemDat == nullptr) {
        std::cout << "[WARN] Item id " << req->Item.iID << " with type " << req->Item.iType << " not found (buy)" << std::endl;
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_ITEM_BUY_FAIL);
        return;
    }

    int itemCost = itemDat->buyPrice * (itemDat->stackSize > 1 ? req->Item.iOpt : 1);
    int slot = Items::findFreeSlot(plr);
    if (itemCost > plr->money || slot == -1) {
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_ITEM_BUY_FAIL);
        return;
    }

    // crates don't have a stack size in TableData, so we can't check those
    if (itemDat->stackSize != 0 && req->Item.iOpt > itemDat->stackSize) {
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_ITEM_BUY_FAIL);
        return;
    }

    if (slot != req->iInvenSlotNum) {
        // possible item stacking?
        std::cout << "[WARN] Client and server disagree on bought item slot (" << req->iInvenSlotNum << " vs " << slot << ")" << std::endl;
    }

    INITSTRUCT(sP_FE2CL_REP_PC_VENDOR_ITEM_BUY_SUCC, resp);

    plr->money = plr->money - itemCost;
    plr->Inven[slot] = req->Item;

    resp.iCandy = plr->money;
    resp.iInvenSlotNum = slot;
    resp.Item = req->Item;

    sock->sendPacket(resp, P_FE2CL_REP_PC_VENDOR_ITEM_BUY_SUCC);
}

static void vendorSell(CNSocket* sock, CNPacketData* data) {
    auto req = (sP_CL2FE_REQ_PC_VENDOR_ITEM_SELL*)data->buf;
    Player* plr = PlayerManager::getPlayer(sock);

    // prepare a fail packet
    INITSTRUCT(sP_FE2CL_REP_PC_VENDOR_ITEM_SELL_FAIL, failResp);
    failResp.iErrorCode = 0;

    if (req->iInvenSlotNum < 0 || req->iInvenSlotNum >= AINVEN_COUNT || req->iItemCnt < 0) {
        std::cout << "[WARN] Client failed to sell item in slot " << req->iInvenSlotNum << std::endl;
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_ITEM_SELL_FAIL);
        return;
    }

    sItemBase* item = &plr->Inven[req->iInvenSlotNum];
    Items::Item* itemData = Items::getItemData(item->iID, item->iType);

    if (itemData == nullptr || !itemData->sellable || item->iOpt < req->iItemCnt) { // sanity + sellable check
        std::cout << "[WARN] Item id " << item->iID << " with type " << item->iType << " not found (sell)" << std::endl;
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_ITEM_SELL_FAIL);
        return;
    }

    // fail to sell croc-potted items
    if (item->iOpt >= 1 << 16) {
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_ITEM_SELL_FAIL);
        return;
    }

    sItemBase original;
    memcpy(&original, item, sizeof(sItemBase));

    INITSTRUCT(sP_FE2CL_REP_PC_VENDOR_ITEM_SELL_SUCC, resp);

    // increment taros
    plr->money += itemData->sellPrice * req->iItemCnt;

    // modify item
    if (item->iOpt - req->iItemCnt > 0) { // selling part of a stack
        item->iOpt -= req->iItemCnt;
        original.iOpt = req->iItemCnt;
    }
    else { // selling entire slot
     // make sure it's fully zeroed, even the padding and non-104 members
        memset(item, 0, sizeof(*item));
    }

    // add to buyback list
    plr->buyback.push_back(original);
    // forget oldest member if there's more than 5
    if (plr->buyback.size() > 5)
        plr->buyback.erase(plr->buyback.begin());
    //std::cout << (int)plr->buyback.size() << " items in buyback\n";

    // response parameters
    resp.iInvenSlotNum = req->iInvenSlotNum;
    resp.iCandy = plr->money;
    resp.Item = original; // the item that gets sent to buyback
    resp.ItemStay = *item; // the void item that gets put in the slot

    sock->sendPacket(resp, P_FE2CL_REP_PC_VENDOR_ITEM_SELL_SUCC);
}

static void vendorBuyback(CNSocket* sock, CNPacketData* data) {
    auto req = (sP_CL2FE_REQ_PC_VENDOR_ITEM_RESTORE_BUY*)data->buf;
    Player* plr = PlayerManager::getPlayer(sock);

    // prepare fail packet
    INITSTRUCT(sP_FE2CL_REP_PC_VENDOR_ITEM_RESTORE_BUY_FAIL, failResp);
    failResp.iErrorCode = 0;

    //std::cout << "buying back from index " << (int)req->iListID << " into " << (int)req->iInvenSlotNum <<
    //    " from " << (int)req->iNPC_ID << " (vendor = " << (int)req->iVendorID << ")\n";

    int idx = req->iListID - 1;

    // sanity check
    if (idx < 0 || idx >= plr->buyback.size()) {
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_ITEM_RESTORE_BUY_FAIL);
        return;
    }

    // get the item out of the buyback list
    sItemBase item = plr->buyback[idx];
    /*
     * NOTE: The client sends the index of the exact item the user clicked on.
     * We then operate on that item, but we remove the *first* identical item
     * from the buyback list, instead of the one at the supplied index.
     *
     * This was originally a mistake on my part, but it turns out the client
     * does the exact same thing, so this *is* the correct thing to do to keep
     * them in sync.
     */
    for (auto it = plr->buyback.begin(); it != plr->buyback.end(); it++) {
        /*
         * XXX: we really need a standard item comparison function that
         * will work properly across all builds (ex. with iSerial)
         */
        if (it->iType == item.iType && it->iID == item.iID && it->iOpt == item.iOpt) {
            plr->buyback.erase(it);
            break;
        }
    }
    //std::cout << (int)plr->buyback.size() << " items in buyback\n";

    Items::Item* itemDat = Items::getItemData(item.iID, item.iType);

    if (itemDat == nullptr) {
        std::cout << "[WARN] Item id " << item.iID << " with type " << item.iType << " not found (rebuy)" << std::endl;
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_ITEM_RESTORE_BUY_FAIL);
        return;
    }

    // sell price is used on rebuy. ternary identifies stacked items
    int itemCost = itemDat->sellPrice * (itemDat->stackSize > 1 ? item.iOpt : 1);
    int slot = Items::findFreeSlot(plr);
    if (itemCost > plr->money || slot == -1) {
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_ITEM_RESTORE_BUY_FAIL);
        return;
    }

    if (slot != req->iInvenSlotNum) {
        // possible item stacking?
        std::cout << "[WARN] Client and server disagree on bought item slot (" << req->iInvenSlotNum << " vs " << slot << ")" << std::endl;
    }

    plr->money = plr->money - itemCost;
    plr->Inven[slot] = item;

    INITSTRUCT(sP_FE2CL_REP_PC_VENDOR_ITEM_RESTORE_BUY_SUCC, resp);
    // response parameters
    resp.iCandy = plr->money;
    resp.iInvenSlotNum = slot;
    resp.Item = item;

    sock->sendPacket(resp, P_FE2CL_REP_PC_VENDOR_ITEM_RESTORE_BUY_SUCC);
}

static void vendorTable(CNSocket* sock, CNPacketData* data) {
    auto req = (sP_CL2FE_REQ_PC_VENDOR_TABLE_UPDATE*)data->buf;

    if (req->iVendorID != req->iNPC_ID || Vendors::VendorTables.find(req->iVendorID) == Vendors::VendorTables.end())
        return;

    std::vector<VendorListing>& listings = Vendors::VendorTables[req->iVendorID];

    INITSTRUCT(sP_FE2CL_REP_PC_VENDOR_TABLE_UPDATE_SUCC, resp);

    for (int i = 0; i < (int)listings.size() && i < 20; i++) { // 20 is the max
        sItemBase base = {};
        base.iID = listings[i].id;
        base.iType = listings[i].type;

        sItemVendor vItem;
        vItem.item = base;
        vItem.iSortNum = listings[i].sort;
        vItem.iVendorID = req->iVendorID;
        //vItem.fBuyCost = listings[i].price; // this value is not actually the one that is used

        resp.item[i] = vItem;
    }

    sock->sendPacket(resp, P_FE2CL_REP_PC_VENDOR_TABLE_UPDATE_SUCC);
}

static void vendorStart(CNSocket* sock, CNPacketData* data) {
    auto req = (sP_CL2FE_REQ_PC_VENDOR_START*)data->buf;
    INITSTRUCT(sP_FE2CL_REP_PC_VENDOR_START_SUCC, resp);

    resp.iNPC_ID = req->iNPC_ID;
    resp.iVendorID = req->iVendorID;

    sock->sendPacket(resp, P_FE2CL_REP_PC_VENDOR_START_SUCC);
}

static void vendorBuyBattery(CNSocket* sock, CNPacketData* data) {
    auto req = (sP_CL2FE_REQ_PC_VENDOR_BATTERY_BUY*)data->buf;
    Player* plr = PlayerManager::getPlayer(sock);

    int cost = req->Item.iOpt * 100;
    if ((req->Item.iID == 3 ? (plr->batteryW >= 9999) : (plr->batteryN >= 9999)) || plr->money < cost || req->Item.iOpt < 0) { // sanity check
        INITSTRUCT(sP_FE2CL_REP_PC_VENDOR_BATTERY_BUY_FAIL, failResp);
        failResp.iErrorCode = 0;
        sock->sendPacket(failResp, P_FE2CL_REP_PC_VENDOR_BATTERY_BUY_FAIL);
        return;
    }

    cost = plr->batteryW + plr->batteryN;
    plr->batteryW += req->Item.iID == 3 ? req->Item.iOpt * 100 : 0;
    plr->batteryN += req->Item.iID == 4 ? req->Item.iOpt * 100 : 0;

    // caps
    if (plr->batteryW > 9999)
        plr->batteryW = 9999;
    if (plr->batteryN > 9999)
        plr->batteryN = 9999;

    cost = plr->batteryW + plr->batteryN - cost;
    plr->money -= cost;

    INITSTRUCT(sP_FE2CL_REP_PC_VENDOR_BATTERY_BUY_SUCC, resp);

    resp.iCandy = plr->money;
    resp.iBatteryW = plr->batteryW;
    resp.iBatteryN = plr->batteryN;

    sock->sendPacket(resp, P_FE2CL_REP_PC_VENDOR_BATTERY_BUY_SUCC);
}

void Vendors::init() {
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_PC_VENDOR_START, vendorStart);
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_PC_VENDOR_TABLE_UPDATE, vendorTable);
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_PC_VENDOR_ITEM_BUY, vendorBuy);
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_PC_VENDOR_ITEM_SELL, vendorSell);
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_PC_VENDOR_ITEM_RESTORE_BUY, vendorBuyback);
    REGISTER_SHARD_PACKET(P_CL2FE_REQ_PC_VENDOR_BATTERY_BUY, vendorBuyBattery);
}
