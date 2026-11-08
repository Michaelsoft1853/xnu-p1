/*
 * Copyright (c) 1998-2012 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2005 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */

const char * gIOKernelConfigTables =
    "("
#if defined(ARM64_BOARD_CONFIG_MSM8994)
    "   {"
    "     'IOClass'         = OP2PlatformExpert;"
    "     'IOProviderClass' = IOPlatformExpertDevice;"
    "     'IONameMatch'     = 'N56AP';"
    "     'IOProbeScore'    = 10000:32;"
    "   },"
    "   {"
    "     'IOClass'         = OP2CPU;"
    "     'IOProviderClass' = IOPlatformDevice;"
    "     'IONameMatch'     = 'arm,cortex-a53';"
    "     'IOProbeScore'    = 10000:32;"
    "   },"
    "   {"
    "     'IOClass'         = AppleKeyStore;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = AppleKeyStore;"
    "   },"
    "   {"
    "     'IOClass'         = IOWatchdog;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = IOWatchdog;"
    "   },"
    "   {"
    "     'IOClass'         = IOCoreSurfaceRoot;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = IOCoreSurfaceRoot;"
    "   },"
    "   {"
    "     'IOClass'         = AppleCLCD;"
    "     'IOProviderClass' = IOPlatformDevice;"
    "     'IONameMatch'     = ('disp0', 'op2,display');"
    "     'IOProbeScore'    = 10000:32;"
    "   },"
    "   {"
    "     'IOClass'         = OP2Digitizer;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = OP2Digitizer;"
    "   },"
    "   {"
    "     'IOClass'         = OP2Buttons;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = OP2Buttons;"
    "   },"
    "   {"
    "     'IOClass'         = OP2GPU;"
    "     'IOProviderClass' = IOResources;"
    "     'IOMatchCategory' = OP2GPU;"
    "   },"
    "   {"
    "     'IOClass'         = OP2I2C;"
    "     'IOProviderClass' = IOPlatformDevice;"
    "     'IONameMatch'     = ('i2c2', 'op2,i2c2');"
    "     'IOProbeScore'    = 10000:32;"
    "   },"
#endif
    "   {"
    "     'IOClass'         = IOPanicPlatform;"
    "     'IOProviderClass' = IOPlatformExpertDevice;"
    "     'IOProbeScore'    = 0:32;"
    "   }"
    ")";

/* This stuff is no longer used at all but was exported in prior
 * releases, so we'll keep them around for PPC/i386 only.
 * See libkern's OSKext.cpp for other symbols, which have been moved
 * there for sanity.
 */
