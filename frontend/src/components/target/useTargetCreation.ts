import { useState, useCallback } from "react";
import type { Target, TargetType } from "../../model/target";
import { useTargetStore } from "../../store/targetStore";

export interface UseTargetCreationReturn {
    selectedTargetType: TargetType;
    setSelectedTargetType: (type: TargetType) => void;
    isCreatingTarget: boolean;
    setIsCreatingTarget: (creating: boolean) => void;
    createTarget: (x: number, y: number, type?: TargetType) => void;
}

export const useTargetCreation = (): UseTargetCreationReturn => {
    const { addTarget } = useTargetStore();

    const [selectedTargetType, setSelectedTargetType] = useState<TargetType>('RECT');
    const [isCreatingTarget, setIsCreatingTarget] = useState(false);

    const createTarget = useCallback((x: number, y: number, type = selectedTargetType) => {
        const baseTarget = {
            type,
            x,
            y,
            color: type === 'RECT' ? '#8B4513' : type === 'CYLINDER' ? '#FF6347' : '#2E8B57',
            angle: 0
        } as Omit<Target, 'id'>;

        if (type === 'RECT') {
            addTarget({
                ...baseTarget,
                w: 50,
                h: 30
            });
        } else if (type === 'CIRCLE') {
            addTarget({
                ...baseTarget,
                r: 20
            });
        } else if (type === 'CYLINDER') {
            addTarget({
                ...baseTarget,
                r: 20,
                h: 40  // 圆柱体高度
            });
        }
    }, [selectedTargetType, addTarget]);

    return {
        selectedTargetType,
        setSelectedTargetType,
        isCreatingTarget,
        setIsCreatingTarget,
        createTarget
    };
};
