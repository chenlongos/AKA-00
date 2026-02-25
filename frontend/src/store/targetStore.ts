import { create } from 'zustand';
import { persist } from 'zustand/middleware';
import type { Target } from '../model/target';
import { INITIAL_TARGETS } from '../model/target';

export type { Target };

interface TargetStore {
    targets: Target[];
    selectedTargetId: string | null;
    addTarget: (target: Omit<Target, 'id'>) => void;
    removeTarget: (id: string) => void;
    updateTarget: (id: string, updates: Partial<Target>) => void;
    setTargets: (targets: Target[]) => void;
    clearTargets: () => void;
    selectTarget: (id: string | null) => void;
    getTargetById: (id: string) => Target | undefined;
    importTargets: (targets: Target[]) => number;
    exportTargets: () => Target[];
}

export const useTargetStore = create<TargetStore>()(
    persist(
        (set, get) => ({
            targets: INITIAL_TARGETS,
            selectedTargetId: null,

            addTarget: (target) => set((state) => ({
                targets: [...state.targets, {
                    ...target,
                    id: Date.now().toString() + Math.random().toString(36).substr(2, 9)
                }]
            })),

            removeTarget: (id) => set((state) => ({
                targets: state.targets.filter(t => t.id !== id),
                selectedTargetId: state.selectedTargetId === id ? null : state.selectedTargetId
            })),

            updateTarget: (id, updates) => set((state) => ({
                targets: state.targets.map(t =>
                    t.id === id ? { ...t, ...updates } : t
                )
            })),

            setTargets: (targets) => set({ targets }),

            clearTargets: () => set({ targets: [], selectedTargetId: null }),

            selectTarget: (id) => set({ selectedTargetId: id }),

            getTargetById: (id) => get().targets.find(t => t.id === id),

            importTargets: (targets) => {
                if (!Array.isArray(targets)) {
                    console.error('导入失败：数据不是数组');
                    return 0;
                }

                const validatedTargets = targets.filter(t => {
                    if (typeof t.id !== 'string' ||
                        typeof t.x !== 'number' ||
                        typeof t.y !== 'number' ||
                        typeof t.color !== 'string' ||
                        typeof t.type !== 'string') {
                        console.warn('跳过无效目标物：缺少必需字段', t);
                        return false;
                    }

                    if (t.type !== 'RECT' && t.type !== 'CIRCLE' && t.type !== 'CYLINDER') {
                        console.warn('跳过无效目标物：type必须为RECT、CIRCLE或CYLINDER', t);
                        return false;
                    }

                    if (t.type === 'RECT') {
                        if (t.w !== undefined && typeof t.w !== 'number') {
                            console.warn('跳过无效目标物：w必须是数字', t);
                            return false;
                        }
                        if (t.h !== undefined && typeof t.h !== 'number') {
                            console.warn('跳过无效目标物：h必须是数字', t);
                            return false;
                        }
                    }

                    if (t.type === 'CIRCLE') {
                        if (t.r !== undefined && typeof t.r !== 'number') {
                            console.warn('跳过无效目标物：r必须是数字', t);
                            return false;
                        }
                    }
                    
                    if (t.type === 'CYLINDER') {
                        if (t.r !== undefined && typeof t.r !== 'number') {
                            console.warn('跳过无效目标物：r必须是数字', t);
                            return false;
                        }
                        if (t.h !== undefined && typeof t.h !== 'number') {
                            console.warn('跳过无效目标物：h必须是数字', t);
                            return false;
                        }
                        // 圆柱体不应有宽度属性
                        if (t.w !== undefined) {
                            console.warn('警告：圆柱体不应有宽度属性，已忽略', t);
                        }
                    }

                    if (t.angle !== undefined && typeof t.angle !== 'number') {
                        console.warn('跳过无效目标物：angle必须是数字', t);
                        return false;
                    }

                    return true;
                });

                console.log(`导入验证通过：${validatedTargets.length}个目标物（跳过${targets.length - validatedTargets.length}个无效项）`);
                set({ targets: validatedTargets });
                return validatedTargets.length;
            },

            exportTargets: () => get().targets,
        }),
        {
            name: 'target-storage-v4',
        }
    )
);
