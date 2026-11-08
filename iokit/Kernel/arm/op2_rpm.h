/*
 * Qualcomm RPM (Resource Power Manager) Client for OnePlus 2 (MSM8994).
 *
 * Implements SMD Edge 15 ("rpm_requests") communication over SMEM to
 * manage PMIC power rails and parent clocks for Adreno 430 GPU bring-up.
 */

#ifndef _BSD_DEV_OP2_RPM_H_
#define _BSD_DEV_OP2_RPM_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Qualcomm RPM Resource Types (4-byte ASCII little-endian) */
#define OP2_RPM_RSC_SMPB    0x62706d73U  /* "smpb" - PMI8994 secondary PMIC SMPS */
#define OP2_RPM_RSC_SMPA    0x61706d73U  /* "smpa" - PM8994 primary PMIC SMPS */
#define OP2_RPM_RSC_LDOA    0x616f646cU  /* "ldoa" - PM8994 LDO */
#define OP2_RPM_RSC_LDOB    0x626f646cU  /* "ldob" - PMI8994 LDO */
#define OP2_RPM_RSC_CLK0    0x306b6c63U  /* "clk0" */
#define OP2_RPM_RSC_CLK1    0x316b6c63U  /* "clk1" */
#define OP2_RPM_RSC_CLK2    0x326b6c63U  /* "clk2" */

/* Parameter Keys (4-byte ASCII little-endian) */
#define OP2_RPM_KEY_SWEN    0x6e657773U  /* "swen" - software enable (0 or 1) */
#define OP2_RPM_KEY_UV      0x00007675U  /* "uv\0\0" - microvolts */
#define OP2_RPM_KEY_CORN    0x6e726f63U  /* "corn" - voltage corner (1..7) */
#define OP2_RPM_KEY_VFC     0x00636676U  /* "vfc\0" - voltage floor corner */
#define OP2_RPM_KEY_KHZ     0x007a484bU  /* "KHz\0" - clock frequency in kHz */

/* Context Sets */
#define OP2_RPM_SET_ACTIVE  0U
#define OP2_RPM_SET_SLEEP   1U

/* MSM8994 Voltage Corner Levels */
#define OP2_RPM_CORNER_NONE         0U
#define OP2_RPM_CORNER_RETENTION    1U
#define OP2_RPM_CORNER_SVS_SOC      2U  /* ~0.900 V (SVS minimum operating level) */
#define OP2_RPM_CORNER_SVS_NOM      3U  /* ~0.950 V (SVS nominal) */
#define OP2_RPM_CORNER_SVS_HIGH     4U  /* ~1.000 V (SVS high) */
#define OP2_RPM_CORNER_NOMINAL      5U  /* ~1.050 V (Nominal) */
#define OP2_RPM_CORNER_TURBO        6U  /* ~1.150 V (Turbo) */
#define OP2_RPM_CORNER_SUPER_TURBO  7U  /* ~1.200 V (Super Turbo) */

/* Core Functions */
bool op2_rpm_init(void);
bool op2_rpm_request(uint32_t set, uint32_t rsc_type, uint32_t rsc_id,
                     const uint32_t *keys, const uint32_t *values, uint32_t nkvps);
bool op2_rpm_set_enable(uint32_t rsc_type, uint32_t rsc_id, bool enable);
bool op2_rpm_set_corner(uint32_t rsc_type, uint32_t rsc_id, uint32_t corner);
bool op2_rpm_set_voltage(uint32_t rsc_type, uint32_t rsc_id, uint32_t uv);

/* Specialized Hardware Power-Rail Operations */
bool op2_rpm_power_on_gfx(void);

#ifdef __cplusplus
}
#endif

#endif /* _BSD_DEV_OP2_RPM_H_ */
