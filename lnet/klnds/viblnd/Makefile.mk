# Copyright (C) 2001  Cluster File Systems, Inc.
#
# This code is issued under the GNU General Public License.
# See the file COPYING in this distribution

include $(src)/../../Kernelenv

obj-y += kvibnal.o
kvibnal-objs := vibnal.o vibnal_cb.o vibnal_sa.o

