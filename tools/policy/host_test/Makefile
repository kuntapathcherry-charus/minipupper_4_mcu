# Host test of the robot's policy arithmetic against omniRL's numpy.
#   make OMNIRL=../../../../omniRL POLICY=bam_t5ub06_stops_v1
OMNIRL ?= ../../../../omniRL
POLICY ?= bam_t5ub06_stops_v1
SRC = ../../../main/rl
MP = $(OMNIRL)/mp3_walk_policy

all: test
host_policy: host_policy.c $(SRC)/policy_core.c $(SRC)/policy_core.h
	$(CC) -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -I$(SRC) host_policy.c $(SRC)/policy_core.c -lm -o $@
test.mpxp:
	python3 ../export_mpxp.py $(OMNIRL)/policies/$(POLICY) --contract $(MP)/data/sku2_contract.json \
	    --mount $(MP)/data/imu_mount.json --robot sku2 --out $@
test: host_policy test.mpxp
	python3 compare.py $(MP) $(OMNIRL)/policies/$(POLICY) test.mpxp ./host_policy
clean:
	rm -f host_policy test.mpxp host_recovery.so rec_walker.mpxp rec_getup.mpxp

# mini_pupper_4_recovery's walker and get-up (FLAG_DECODER):
#   make recovery RECOVERY=C:/esp6/projects/mini_pupper_4_recovery
RECOVERY ?= ../../../../../../esp6/projects/mini_pupper_4_recovery
RDATA = $(RECOVERY)/src/mp4_recovery/data
host_recovery.so: host_recovery.c $(SRC)/policy_core.c $(SRC)/policy_core.h
	$(CC) -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -shared -fPIC -I$(SRC) host_recovery.c $(SRC)/policy_core.c -lm -o $@
recovery: host_recovery.so
	python3 ../export_mpxp.py $(RECOVERY)/policies/walker --contract $(RDATA)/sku2_contract.json \
	    --mount $(RDATA)/imu_mount.json --robot sku2 --out rec_walker.mpxp
	python3 ../export_mpxp.py $(RECOVERY)/policies/getup --contract $(RDATA)/sku2_contract.json \
	    --mount $(RDATA)/imu_mount.json --robot sku2 --out rec_getup.mpxp
	python3 compare_recovery.py $(RECOVERY) ./host_recovery.so walker rec_walker.mpxp
	python3 compare_recovery.py $(RECOVERY) ./host_recovery.so getup rec_getup.mpxp
.PHONY: all test clean recovery
