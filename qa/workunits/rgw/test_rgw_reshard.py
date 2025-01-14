#!/usr/bin/python3

import time
import logging as log
import json
import os
from common import exec_cmd, boto_connect, create_user, put_objects, create_unlinked_objects

"""
Rgw manual and dynamic resharding  testing against a running instance
"""
# The test cases in this file have been annotated for inventory.
# To extract the inventory (in csv format) use the command:
#
#   grep '^ *# TESTCASE' | sed 's/^ *# TESTCASE //'
#
#

""" Constants """
USER = 'tester'
DISPLAY_NAME = 'Testing'
ACCESS_KEY = 'NX5QOQKC6BH2IDN8HC7A'
SECRET_KEY = 'LnEsqNNqZIpkzauboDcLXLcYaWwLQ3Kop0zAnKIn'
BUCKET_NAME1 = 'myfoo'
BUCKET_NAME2 = 'mybar'
VER_BUCKET_NAME = 'myver'
INDEX_POOL = 'default.rgw.buckets.index'

class BucketStats:
    def __init__(self, bucket_name, bucket_id, num_objs=0, size_kb=0, num_shards=0):
        self.bucket_name = bucket_name
        self.bucket_id = bucket_id
        self.num_objs = num_objs
        self.size_kb = size_kb
        self.num_shards = num_shards if num_shards > 0 else 1

    def get_num_shards(self):
        self.num_shards = get_bucket_num_shards(self.bucket_name, self.bucket_id)


def get_bucket_stats(bucket_name):
    """
    function to get bucket stats
    """
    cmd = exec_cmd("radosgw-admin bucket stats --bucket %s" % bucket_name)
    json_op = json.loads(cmd)
    bucket_id = json_op['id']
    num_shards_op = json_op['num_shards']
    if len(json_op['usage']) > 0:
        num_objects = json_op['usage']['rgw.main']['num_objects']
        size_kb = json_op['usage']['rgw.main']['size_kb']
    else:
        num_objects = 0
        size_kb = 0
    log.debug("bucket %s id %s num_objects %d size_kb %d num_shards %d", bucket_name, bucket_id,
              num_objects, size_kb, num_shards_op)
    return BucketStats(bucket_name, bucket_id, num_objects, size_kb, num_shards_op)


def get_bucket_num_shards(bucket_name, bucket_id):
    """
    function to get bucket num shards
    """
    metadata = 'bucket.instance:' + bucket_name + ':' + bucket_id
    log.debug("metadata %s", metadata)
    cmd = exec_cmd('radosgw-admin metadata get %s' % metadata)
    json_op = json.loads(cmd)
    num_shards = json_op['data']['bucket_info']['num_shards']
    log.debug("bucket %s id %s num_shards %d", bucket_name, bucket_id, num_shards)
    return num_shards


def main():
    """
    execute manual and dynamic resharding commands
    """
    create_user(USER, DISPLAY_NAME, ACCESS_KEY, SECRET_KEY)

    connection = boto_connect(ACCESS_KEY, SECRET_KEY)

    # create a bucket
    bucket1 = connection.create_bucket(Bucket=BUCKET_NAME1)
    bucket2 = connection.create_bucket(Bucket=BUCKET_NAME2)
    ver_bucket = connection.create_bucket(Bucket=VER_BUCKET_NAME)
    connection.BucketVersioning(VER_BUCKET_NAME).enable()

    bucket_stats1 = get_bucket_stats(BUCKET_NAME1)
    bucket_stats2 = get_bucket_stats(BUCKET_NAME2)
    ver_bucket_stats = get_bucket_stats(VER_BUCKET_NAME)

    bucket1_acl = connection.BucketAcl(BUCKET_NAME1).load()
    bucket2_acl = connection.BucketAcl(BUCKET_NAME2).load()
    ver_bucket_acl = connection.BucketAcl(VER_BUCKET_NAME).load()

    # TESTCASE 'reshard-add','reshard','add','add bucket to resharding queue','succeeds'
    log.debug(' test: reshard add')
    num_shards_expected = bucket_stats1.num_shards + 1
    cmd = exec_cmd('radosgw-admin reshard add --bucket %s --num-shards %s' % (BUCKET_NAME1, num_shards_expected))
    cmd = exec_cmd('radosgw-admin reshard list')
    json_op = json.loads(cmd)
    log.debug('bucket name %s', json_op[0]['bucket_name'])
    assert json_op[0]['bucket_name'] == BUCKET_NAME1
    assert json_op[0]['tentative_new_num_shards'] == num_shards_expected

    # TESTCASE 'reshard-process','reshard','','process bucket resharding','succeeds'
    log.debug(' test: reshard process')
    cmd = exec_cmd('radosgw-admin reshard process')
    time.sleep(5)
    # check bucket shards num
    bucket_stats1 = get_bucket_stats(BUCKET_NAME1)
    bucket_stats1.get_num_shards()
    if bucket_stats1.num_shards != num_shards_expected:
        log.error("Resharding failed on bucket %s. Expected number of shards are not created" % BUCKET_NAME1)

    # TESTCASE 'reshard-add','reshard','add','add non empty bucket to resharding queue','succeeds'
    log.debug(' test: reshard add non empty bucket')
    # create objs
    num_objs = 8
    for i in range(0, num_objs):
        connection.Object(BUCKET_NAME1, ('key'+str(i))).put(Body=b"some_data")

    num_shards_expected = bucket_stats1.num_shards + 1
    cmd = exec_cmd('radosgw-admin reshard add --bucket %s --num-shards %s' % (BUCKET_NAME1, num_shards_expected))
    cmd = exec_cmd('radosgw-admin reshard list')
    json_op = json.loads(cmd)
    log.debug('bucket name %s', json_op[0]['bucket_name'])
    assert json_op[0]['bucket_name'] == BUCKET_NAME1
    assert json_op[0]['tentative_new_num_shards'] == num_shards_expected

    # TESTCASE 'reshard process ,'reshard','process','reshard non empty bucket','succeeds'
    log.debug(' test: reshard process non empty bucket')
    cmd = exec_cmd('radosgw-admin reshard process')
    # check bucket shards num
    bucket_stats1 = get_bucket_stats(BUCKET_NAME1)
    bucket_stats1.get_num_shards()
    if bucket_stats1.num_shards != num_shards_expected:
        log.error("Resharding failed on bucket %s. Expected number of shards are not created" % BUCKET_NAME1)

    # TESTCASE 'manual resharding','bucket', 'reshard','','manual bucket resharding','succeeds'
    log.debug(' test: manual reshard bucket')
    # create objs
    num_objs = 11
    for i in range(0, num_objs):
        connection.Object(BUCKET_NAME2, ('key' + str(i))).put(Body=b"some_data")

    time.sleep(10)
    num_shards_expected = bucket_stats2.num_shards + 1
    cmd = exec_cmd('radosgw-admin bucket reshard --bucket %s --num-shards %s' % (BUCKET_NAME2,
                                                                                 num_shards_expected))
    # check bucket shards num
    bucket_stats2 = get_bucket_stats(BUCKET_NAME2)
    bucket_stats2.get_num_shards()
    if bucket_stats2.num_shards != num_shards_expected:
        log.error("Resharding failed on bucket %s. Expected number of shards are not created" % BUCKET_NAME2)

    # TESTCASE 'versioning reshard-','bucket', reshard','versioning reshard','succeeds'
    log.debug(' test: reshard versioned bucket')
    num_shards_expected = ver_bucket_stats.num_shards + 1
    cmd = exec_cmd('radosgw-admin bucket reshard --bucket %s --num-shards %s' % (VER_BUCKET_NAME,
                                                                                 num_shards_expected))
    # check bucket shards num
    ver_bucket_stats = get_bucket_stats(VER_BUCKET_NAME)
    assert ver_bucket_stats.num_shards == num_shards_expected

    # TESTCASE 'check acl'
    new_bucket1_acl = connection.BucketAcl(BUCKET_NAME1).load()
    assert new_bucket1_acl == bucket1_acl
    new_bucket2_acl = connection.BucketAcl(BUCKET_NAME2).load()
    assert new_bucket2_acl == bucket2_acl
    new_ver_bucket_acl = connection.BucketAcl(VER_BUCKET_NAME).load()
    assert new_ver_bucket_acl == ver_bucket_acl

    # TESTCASE 'check reshard removes olh entries with empty name'
    log.debug(' test: reshard removes olh entries with empty name')
    bucket1.objects.all().delete()

    # get name of shard 0 object, add a bogus olh entry with empty name
    bucket_shard0 = '.dir.%s.0' % get_bucket_stats(BUCKET_NAME1).bucket_id
    if 'CEPH_ROOT' in os.environ:
      k = '%s/qa/workunits/rgw/olh_noname_key' % os.environ['CEPH_ROOT']
      v = '%s/qa/workunits/rgw/olh_noname_val' % os.environ['CEPH_ROOT']
    else:
      k = 'olh_noname_key'
      v = 'olh_noname_val'
    exec_cmd('rados -p %s setomapval %s --omap-key-file %s < %s' % (INDEX_POOL, bucket_shard0, k, v))

    # check that bi list has one entry with empty name
    cmd = exec_cmd('radosgw-admin bi list --bucket %s' % BUCKET_NAME1)
    json_op = json.loads(cmd.decode('utf-8', 'ignore')) # ignore utf-8 can't decode 0x80
    assert len(json_op) == 1
    assert json_op[0]['entry']['key']['name'] == ''

    # reshard to prune the bogus olh
    cmd = exec_cmd('radosgw-admin bucket reshard --bucket %s --num-shards %s --yes-i-really-mean-it' % (BUCKET_NAME1, 1))

    # get new name of shard 0 object, check that bi list has zero entries
    bucket_shard0 = '.dir.%s.0' % get_bucket_stats(BUCKET_NAME1).bucket_id
    cmd = exec_cmd('radosgw-admin bi list --bucket %s' % BUCKET_NAME1)
    json_op = json.loads(cmd)
    assert len(json_op) == 0

    # TESTCASE 'check that PUT succeeds during reshard'
    log.debug(' test: PUT succeeds during reshard')
    num_shards = get_bucket_stats(VER_BUCKET_NAME).num_shards
    exec_cmd('''radosgw-admin --inject-delay-at=do_reshard --inject-delay-ms=5000 \
                bucket reshard --bucket {} --num-shards {}'''
                .format(VER_BUCKET_NAME, num_shards + 1), wait = False)
    time.sleep(1)
    ver_bucket.put_object(Key='put_during_reshard', Body=b"some_data")
    log.debug('put object successful')

    # TESTCASE 'check that bucket stats are correct after reshard with unlinked entries'
    log.debug('TEST: check that bucket stats are correct after reshard with unlinked entries\n')
    ver_bucket.object_versions.all().delete()
    ok_keys = ['a', 'b', 'c']
    unlinked_keys = ['x', 'y', 'z']
    put_objects(ver_bucket, ok_keys)
    create_unlinked_objects(connection, ver_bucket, unlinked_keys)
    cmd = exec_cmd(f'radosgw-admin bucket reshard --bucket {VER_BUCKET_NAME} --num-shards 17 --yes-i-really-mean-it')
    out = exec_cmd(f'radosgw-admin bucket check unlinked --bucket {VER_BUCKET_NAME} --fix --min-age-hours 0 --rgw-olh-pending-timeout-sec 0 --dump-keys')
    json_out = json.loads(out)
    assert len(json_out) == len(unlinked_keys)
    ver_bucket.object_versions.all().delete()
    out = exec_cmd(f'radosgw-admin bucket stats --bucket {VER_BUCKET_NAME}')
    json_out = json.loads(out)
    log.debug(json_out['usage'])
    assert json_out['usage']['rgw.main']['size'] == 0
    assert json_out['usage']['rgw.main']['num_objects'] == 0
    assert json_out['usage']['rgw.main']['size_actual'] == 0
    assert json_out['usage']['rgw.main']['size_kb'] == 0
    assert json_out['usage']['rgw.main']['size_kb_actual'] == 0
    assert json_out['usage']['rgw.main']['size_kb_utilized'] == 0

    # Clean up
    log.debug("Deleting bucket %s", BUCKET_NAME1)
    bucket1.objects.all().delete()
    bucket1.delete()
    log.debug("Deleting bucket %s", BUCKET_NAME2)
    bucket2.objects.all().delete()
    bucket2.delete()
    log.debug("Deleting bucket %s", VER_BUCKET_NAME)
    ver_bucket.delete()


main()
log.info("Completed resharding tests")
