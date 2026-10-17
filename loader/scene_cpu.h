/* scene_cpu.h -- light lists without the scans, animation of gobs not drawn
 * this frame one frame in ANIM_LOD_N, scene timing (scene_cpu.c). */
#ifndef SCENE_CPU_H
#define SCENE_CPU_H

/* Takes LightManager::SetupLightsForThisPositionAndRadius and Gob::Animate
 * (scene_mode.txt); with SCENE_CPU_STATS also times ManageSceneBSP and
 * Scene::DoGobBuckets. */
void scene_cpu_install(void);

/* At the swap, game thread: the A/B and the [scene] report every 10 s. */
void scene_cpu_on_swap(void);

/* -1 not installed, 1 on, 0 off (the A/B's other half). */
int scene_cpu_phase(void);

#endif
