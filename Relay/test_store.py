"""Unit test of the relay's checkpoint store (no network). Usage: python test_store.py"""
import os, struct, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import coop_relay as relay

OWNER = "6dbc17f1d8401351"


def blob_file(folder, campaign, name):
    progress = ("campaign=%s\nname=%s\nlevel=rescue\ncheckpoint=rescue_start\nstamp=%d\nplayer=id:%s\tx\n"
                % (campaign, name, int(time.time()), OWNER)).encode()
    save = b"save" * 10
    data = relay.MAGIC_BLOB + b"\0" * 4 + struct.pack("<I", len(progress)) + progress + struct.pack("<I", len(save)) + save
    path = os.path.join(folder, campaign + ".tmp")
    with open(path, "wb") as f:
        f.write(data)
    return path, len(data)


def main():
    root = tempfile.mkdtemp(prefix="coop_store_")
    store = relay.Store(root)
    ids = ["%016x" % (0xa000 + i) for i in range(relay.MAX_CAMPAIGNS + 2)]
    for i, campaign in enumerate(ids):
        os.makedirs(os.path.join(store.campaign_dir, campaign), exist_ok=True)
        path, size = blob_file(root, campaign, "Campaign %d" % i)
        error = store.store(campaign, OWNER, path, size)
        assert error == 0, "campaign %d refused with %d" % (i, error)
        store.campaigns[campaign]["updated"] = 1000.0 + i     # i = order of the last checkpoint
    own = sorted(c for c, m in store.campaigns.items() if m["owner"] == OWNER)
    assert len(own) == relay.MAX_CAMPAIGNS, own
    assert ids[0] not in own and ids[1] not in own, "the two least recently played ones should have gone"
    assert not os.path.exists(os.path.join(store.campaign_dir, ids[0])), "its folder should be gone"
    # an old campaign that gets a new checkpoint is kept, the next oldest goes instead
    store.campaigns[ids[2]]["updated"] = 5000.0
    new = "%016x" % 0xbeef
    os.makedirs(os.path.join(store.campaign_dir, new), exist_ok=True)
    path, size = blob_file(root, new, "Newest")
    assert store.store(new, OWNER, path, size) == 0
    assert ids[2] in store.campaigns and ids[3] not in store.campaigns
    print("store test passed")


if __name__ == "__main__":
    main()
