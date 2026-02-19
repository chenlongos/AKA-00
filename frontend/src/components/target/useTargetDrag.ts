import { useEffect, useRef, useCallback } from "react";
import type { Target } from "../../model/target";
import { getTargetAtPosition } from "../../model/target";

interface DraggingState {
    isDragging: boolean;
    targetId: string | null;
}

interface UseTargetDragProps {
    canvasRef: React.RefObject<HTMLCanvasElement | null>;
    targetsRef: React.MutableRefObject<Target[]>;
    updateTarget: (id: string, updates: Partial<Target>) => void;
    selectTarget: (id: string | null) => void;
    isCreatingTarget: boolean;
    createTarget: (x: number, y: number) => void;
}

export const useTargetDrag = ({
    canvasRef,
    targetsRef,
    updateTarget,
    selectTarget,
    isCreatingTarget,
    createTarget
}: UseTargetDragProps): void => {
    const draggingRef = useRef<DraggingState>({
        isDragging: false,
        targetId: null
    });

    const getCanvasCoordinates = useCallback((e: MouseEvent) => {
        const canvas = canvasRef.current;
        if (!canvas) return { x: 0, y: 0 };

        const rect = canvas.getBoundingClientRect();
        return {
            x: e.clientX - rect.left,
            y: e.clientY - rect.top
        };
    }, [canvasRef]);

    const handleMouseDown = useCallback((e: MouseEvent) => {
        const canvas = canvasRef.current;
        if (!canvas) return;

        const { x, y } = getCanvasCoordinates(e);

        if (isCreatingTarget) {
            createTarget(x, y);
        } else {
            const clickedTarget = getTargetAtPosition(x, y, targetsRef.current);
            if (clickedTarget) {
                selectTarget(clickedTarget.id);
                draggingRef.current = {
                    isDragging: true,
                    targetId: clickedTarget.id
                };
            } else {
                selectTarget(null);
            }
        }
    }, [canvasRef, getCanvasCoordinates, isCreatingTarget, createTarget, targetsRef, selectTarget]);

    const handleMouseMove = useCallback((e: MouseEvent) => {
        const canvas = canvasRef.current;
        if (!canvas || !draggingRef.current.isDragging || !draggingRef.current.targetId) return;

        const { x, y } = getCanvasCoordinates(e);
        updateTarget(draggingRef.current.targetId, { x, y });
    }, [canvasRef, getCanvasCoordinates, updateTarget]);

    const handleMouseUp = useCallback(() => {
        draggingRef.current = {
            isDragging: false,
            targetId: null
        };
    }, []);

    useEffect(() => {
        const canvas = canvasRef.current;
        if (!canvas) return;

        canvas.addEventListener('mousedown', handleMouseDown);
        canvas.addEventListener('mousemove', handleMouseMove);
        canvas.addEventListener('mouseup', handleMouseUp);
        canvas.addEventListener('mouseleave', handleMouseUp);

        return () => {
            canvas.removeEventListener('mousedown', handleMouseDown);
            canvas.removeEventListener('mousemove', handleMouseMove);
            canvas.removeEventListener('mouseup', handleMouseUp);
            canvas.removeEventListener('mouseleave', handleMouseUp);
        };
    }, [canvasRef, handleMouseDown, handleMouseMove, handleMouseUp]);
};
