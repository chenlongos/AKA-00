import {useState} from "react";
import type {Target, TargetType} from "../../model/target";
import {useTargetStore} from "../../store/targetStore";
import ModalImportDialog from "./ModalImportDialog";

interface TargetManagerProps {
    onCreateInFront?: (x: number, y: number) => void;
    isCreatingTarget?: boolean;
    onToggleCreating?: (creating: boolean) => void;
    selectedTargetType?: TargetType;
    onTargetTypeChange?: (type: TargetType) => void;
}

export const TargetManager: React.FC<TargetManagerProps> = ({
    onCreateInFront,
    isCreatingTarget: externalIsCreating,
    onToggleCreating,
    selectedTargetType: externalSelectedType,
    onTargetTypeChange
}) => {
    const {
        targets,
        updateTarget,
        removeTarget,
        selectTarget,
        selectedTargetId,
        exportTargets
    } = useTargetStore();

    const [editingTarget, setEditingTarget] = useState<Target | null>(null);
    const [editForm, setEditForm] = useState<Partial<Target>>({});
    const [showImportDialog, setShowImportDialog] = useState(false);

    const handleExportTargets = () => {
        const targetsData = exportTargets();
        const dataStr = JSON.stringify(targetsData, null, 2);
        const dataBlob = new Blob([dataStr], { type: 'application/json' });
        const url = URL.createObjectURL(dataBlob);
        const link = document.createElement('a');
        link.href = url;
        link.download = `targets_${new Date().toISOString().slice(0, 10)}.json`;
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        URL.revokeObjectURL(url);
    };



    const isCreatingTarget = externalIsCreating !== undefined ? externalIsCreating : false;
    const selectedTargetType = externalSelectedType || 'RECT';
    const setSelectedTargetType = (type: TargetType) => {
        onTargetTypeChange?.(type);
    };

    const handleSaveEdit = () => {
        if (editingTarget) {
            updateTarget(editingTarget.id, editForm);
            setEditingTarget(null);
            setEditForm({});
        }
    };

    const handleCancelEdit = () => {
        setEditingTarget(null);
        setEditForm({});
    };

    const handleDelete = (id: string) => {
        removeTarget(id);
        if (selectedTargetId === id) {
            selectTarget(null);
        }
    };

    const handleStartEdit = (t: Target) => {
        setEditingTarget(t);
        setEditForm({...t});
    };

    const handleCreateTargetAtCamera = () => {
        if (onCreateInFront) {
            onCreateInFront(0, 0);
        }
    };

    return (
        <div style={{
            width: '100%',
            padding: 0,
            background: 'transparent',
            color: '#1f2937',
            display: 'flex',
            flexDirection: 'column',
            height: '100%'
        }}>
            <h3 style={{
                marginTop: 0,
                marginBottom: '10px',
                color: '#111827',
                fontSize: '14px',
                fontWeight: 600,
                background: '#eef2f7',
                padding: '6px 8px',
                borderRadius: 6
            }}>目标物管理</h3>

            <div style={{marginTop: '10px', display: 'flex', flexDirection: 'column', gap: 6, flex: 1, minHeight: 0}}>
                <h4 style={{marginTop: 0, marginBottom: '8px', fontSize: '13px', color: '#374151'}}>已有目标物列表</h4>
                <div style={{display: 'flex', gap: '8px', marginBottom: '8px', flexWrap: 'wrap'}}>
                    <button
                        onClick={() => handleExportTargets()}
                        style={{
                            padding: '4px 8px',
                            fontSize: '12px',
                            backgroundColor: '#4ecdc4',
                            color: 'white',
                            border: 'none',
                            borderRadius: '3px',
                            cursor: 'pointer',
                            transition: 'background-color 0.2s'
                        }}
                        onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#45b7d1'}
                        onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#4ecdc4'}
                    >
                        导出为JSON
                    </button>
                    <button
                        onClick={() => setShowImportDialog(true)}
                        style={{
                            padding: '4px 8px',
                            fontSize: '12px',
                            backgroundColor: '#3498db',
                            color: 'white',
                            border: 'none',
                            borderRadius: '3px',
                            cursor: 'pointer',
                            transition: 'background-color 0.2s'
                        }}
                        onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#2980b9'}
                        onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#3498db'}
                    >
                        导入JSON
                    </button>

                </div>
                <div style={{
                    border: '1px solid #e5e7eb',
                    borderRadius: '4px',
                    padding: '6px',
                    flex: 1,
                    minHeight: 0,
                    overflowY: 'auto',
                    background: '#f8fafc'
                }}>
                    {editingTarget ? (
                        <TargetEditForm
                            editingTarget={editingTarget}
                            editForm={editForm}
                            setEditForm={setEditForm}
                            onSave={handleSaveEdit}
                            onCancel={handleCancelEdit}
                        />
                    ) : (
                        targets.length > 0 ? (
                            <div style={{
                                display: 'grid',
                                gridTemplateColumns: '1fr 1fr',
                                gap: '10px'
                            }}>
                                {targets.map(t => (
                                    <TargetItem
                                        key={t.id}
                                        target={t}
                                        isSelected={t.id === selectedTargetId}
                                        onSelect={() => selectTarget(t.id)}
                                        onEdit={() => handleStartEdit(t)}
                                        onDelete={() => handleDelete(t.id)}
                                    />
                                ))}
                            </div>
                        ) : (
                            <div style={{textAlign: 'center', color: '#6b7280', padding: '15px', fontSize: '13px'}}>
                                暂无目标物数据
                            </div>
                        )
                    )}
                </div>
            </div>

            <div style={{marginTop: '8px'}}>
                <TargetCreator
                    selectedTargetType={selectedTargetType}
                    setSelectedTargetType={setSelectedTargetType}
                    isCreatingTarget={isCreatingTarget}
                    onToggleCreating={onToggleCreating}
                    onCreateAtCamera={handleCreateTargetAtCamera}
                />
            </div>

            <ModalImportDialog
                isOpen={showImportDialog}
                onClose={() => setShowImportDialog(false)}
            />
        </div>
    );
};

interface TargetEditFormProps {
    editingTarget: Target;
    editForm: Partial<Target>;
    setEditForm: (form: Partial<Target>) => void;
    onSave: () => void;
    onCancel: () => void;
}

const TargetEditForm: React.FC<TargetEditFormProps> = ({
    editingTarget,
    editForm,
    setEditForm,
    onSave,
    onCancel
}) => {
    const [fieldErrors, setFieldErrors] = useState<Record<string, string>>({});

    const handleNumberChange = (field: keyof Target, value: string) => {
        const num = parseFloat(value);
        let adjusted = num;
        let error = '';

        if (isNaN(num)) {
            adjusted = 0;
        } else if (num <= 0) {
            if (field === 'w' || field === 'h' || field === 'r') {
                adjusted = 1;
                error = '值必须大于0，已自动调整为1';
            }
        }

        setEditForm({...editForm, [field]: adjusted});

        if (error) {
            setFieldErrors({...fieldErrors, [field]: error});
            setTimeout(() => {
                setFieldErrors(prev => ({...prev, [field]: ''}));
            }, 3000);
        } else {
            setFieldErrors(prev => ({...prev, [field]: ''}));
        }
    };

    return (
        <div style={{
            padding: '12px',
            border: '1px solid #4ecdc4',
            borderRadius: '4px',
            background: 'rgb(255, 255, 255)',
            boxShadow: '0 2px 4px rgba(0,0,0,0.1)',
            marginBottom: '10px'
        }}>
            <h5 style={{marginTop: 0, marginBottom: '10px', color: '#4ecdc4'}}>编辑目标物</h5>
            <div style={{display: 'flex', flexDirection: 'column', gap: '8px', marginTop: '10px'}}>
                <div>
                    <label style={{fontSize: '12px', marginRight: '5px'}}>X坐标: </label>
                    <input
                        type="number"
                        value={editForm.x}
                        onChange={(e) => handleNumberChange('x', e.target.value)}
                        style={{fontSize: '12px', padding: '3px 5px', width: '80px'}}
                    />
                </div>
                <div>
                    <label style={{fontSize: '12px', marginRight: '5px'}}>Y坐标: </label>
                    <input
                        type="number"
                        value={editForm.y}
                        onChange={(e) => handleNumberChange('y', e.target.value)}
                        style={{fontSize: '12px', padding: '3px 5px', width: '80px'}}
                    />
                </div>
                {editingTarget.type === 'RECT' && (
                    <>
                        <div>
                            <label style={{fontSize: '12px', marginRight: '5px'}}>宽度: </label>
                            <input
                                type="number"
                                value={editForm.w}
                                onChange={(e) => handleNumberChange('w', e.target.value)}
                                style={{fontSize: '12px', padding: '3px 5px', width: '60px'}}
                            />
                            {fieldErrors['w'] && (
                                <div style={{fontSize: '10px', color: '#e74c3c', marginTop: '2px'}}>
                                    {fieldErrors['w']}
                                </div>
                            )}
                        </div>
                        <div>
                            <label style={{fontSize: '12px', marginRight: '5px'}}>长度: </label>
                            <input
                                type="number"
                                value={editForm.h}
                                onChange={(e) => handleNumberChange('h', e.target.value)}
                                style={{fontSize: '12px', padding: '3px 5px', width: '60px'}}
                            />
                            {fieldErrors['h'] && (
                                <div style={{fontSize: '10px', color: '#e74c3c', marginTop: '2px'}}>
                                    {fieldErrors['h']}
                                </div>
                            )}
                        </div>
                    </>
                )}
                {(editingTarget.type === 'CIRCLE' || editingTarget.type === 'CYLINDER') && (
                    <div>
                        <label style={{fontSize: '12px', marginRight: '5px'}}>半径: </label>
                        <input
                            type="number"
                            value={editForm.r}
                            onChange={(e) => handleNumberChange('r', e.target.value)}
                            style={{fontSize: '12px', padding: '3px 5px', width: '60px'}}
                        />
                        {fieldErrors['r'] && (
                            <div style={{fontSize: '10px', color: '#e74c3c', marginTop: '2px'}}>
                                {fieldErrors['r']}
                            </div>
                        )}
                    </div>
                )}
                {editingTarget.type === 'CYLINDER' && (
                    <div>
                        <label style={{fontSize: '12px', marginRight: '5px'}}>高度: </label>
                        <input
                            type="number"
                            value={editForm.h}
                            onChange={(e) => handleNumberChange('h', e.target.value)}
                            style={{fontSize: '12px', padding: '3px 5px', width: '60px'}}
                        />
                        {fieldErrors['h'] && (
                            <div style={{fontSize: '10px', color: '#e74c3c', marginTop: '2px'}}>
                                {fieldErrors['h']}
                            </div>
                        )}
                    </div>
                )}
                {(editingTarget.type === 'RECT' || editingTarget.type === 'CYLINDER') && (
                    <div>
                        <label style={{fontSize: '12px', marginRight: '5px'}}>旋转角度: </label>
                        <input
                            type="number"
                            step="0.1"
                            value={editForm.angle || 0}
                            onChange={(e) => handleNumberChange('angle', e.target.value)}
                            style={{fontSize: '12px', padding: '3px 5px', width: '80px'}}
                        />
                        {fieldErrors['angle'] && (
                            <div style={{fontSize: '10px', color: '#e74c3c', marginTop: '2px'}}>
                                {fieldErrors['angle']}
                            </div>
                        )}
                    </div>
                )}
                <div>
                    <label style={{fontSize: '12px', marginRight: '5px'}}>颜色: </label>
                    <input
                        type="color"
                        value={editForm.color}
                        onChange={(e) => setEditForm({...editForm, color: e.target.value})}
                        style={{width: '40px', height: '20px', padding: 0, border: '1px solid #ddd'}}
                    />
                </div>
                <div style={{display: 'flex', gap: '8px', marginTop: '10px'}}>
                    <button onClick={onSave} style={{
                        padding: '5px 10px',
                        fontSize: '12px',
                        backgroundColor: '#4ecdc4',
                        color: 'white',
                        border: 'none',
                        borderRadius: '3px',
                        cursor: 'pointer',
                        transition: 'background-color 0.2s'
                    }} onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#45b7d1'}
                      onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#4ecdc4'}>
                        保存修改
                    </button>
                    <button onClick={onCancel} style={{
                        padding: '5px 10px',
                        fontSize: '12px',
                        backgroundColor: '#95a5a6',
                        color: 'white',
                        border: 'none',
                        borderRadius: '3px',
                        cursor: 'pointer',
                        transition: 'background-color 0.2s'
                    }} onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#7f8c8d'}
                      onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#95a5a6'}>
                        取消
                    </button>
                </div>
            </div>
        </div>
    );
};

interface TargetItemProps {
    target: Target;
    isSelected: boolean;
    onSelect: () => void;
    onEdit: () => void;
    onDelete: () => void;
}

const TargetItem: React.FC<TargetItemProps> = ({
    target,
    isSelected,
    onSelect,
    onEdit,
    onDelete
}) => {
    return (
        <div
            style={{
                border: `1px solid ${isSelected ? '#4ecdc4' : '#e5e7eb'}`,
                borderRadius: '4px',
                padding: '5px',
                background: isSelected ? 'rgba(78, 205, 196, 0.1)' : '#ffffff',
                cursor: 'pointer',
                transition: 'all 0.2s ease',
                minHeight: '70px',
                display: 'flex',
                flexDirection: 'column'
            }}
            onMouseEnter={(e) => {
                e.currentTarget.style.boxShadow = '0 2px 4px rgba(0,0,0,0.1)';
                e.currentTarget.style.transform = 'translateY(-1px)';
            }}
            onMouseLeave={(e) => {
                e.currentTarget.style.boxShadow = 'none';
                e.currentTarget.style.transform = 'translateY(0)';
            }}
            onClick={onSelect}
        >
            <div style={{display: 'flex', alignItems: 'center', gap: '4px', marginBottom: '1px'}}>
                <div style={{width: '12px', height: '12px', backgroundColor: target.color, borderRadius: '2px', border: '1px solid #ddd'}} />
                <span style={{fontSize: '10px', color: '#374151', fontWeight: '600'}}>
                    {target.type === 'RECT' ? '矩形' : target.type === 'CYLINDER' ? '圆柱体' : '圆形'}
                </span>
            </div>

            <div style={{fontSize: '9px', color: '#6b7280', marginBottom: '1px', whiteSpace: 'normal', wordBreak: 'break-all', lineHeight: '1.1'}}>
                ID: {target.id}
            </div>

            <div style={{fontSize: '9px', color: '#4b5563', marginBottom: '1px', lineHeight: '1.1'}}>
                位置: ({Math.floor(target.x)}, {Math.floor(target.y)})
            </div>

            <div style={{fontSize: '9px', color: '#4b5563', marginBottom: '1px', lineHeight: '1.1'}}>
                {target.type === 'RECT' ? `尺寸: ${target.w}x${target.h}` : 
                 target.type === 'CYLINDER' ? `半径: ${target.r}, 高: ${target.h}` : 
                 `半径: ${target.r}`}
            </div>

            <div style={{display: 'flex', justifyContent: 'flex-end', gap: '4px', marginTop: '2px'}}>
                <button
                    onClick={(e) => {
                        e.stopPropagation();
                        onEdit();
                    }}
                    style={{
                        padding: '2px 5px',
                        fontSize: '8px',
                        backgroundColor: '#f39c12',
                        color: 'white',
                        border: 'none',
                        borderRadius: '3px',
                        cursor: 'pointer',
                        transition: 'background-color 0.2s'
                    }}
                    onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#e67e22'}
                    onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#f39c12'}
                >
                    编辑
                </button>
                <button
                    onClick={(e) => {
                        e.stopPropagation();
                        onDelete();
                    }}
                    style={{
                        padding: '2px 5px',
                        fontSize: '8px',
                        backgroundColor: '#e74c3c',
                        color: 'white',
                        border: 'none',
                        borderRadius: '3px',
                        cursor: 'pointer',
                        transition: 'background-color 0.2s'
                    }}
                    onMouseEnter={(e) => e.currentTarget.style.backgroundColor = '#c0392b'}
                    onMouseLeave={(e) => e.currentTarget.style.backgroundColor = '#e74c3c'}
                >
                    删除
                </button>
            </div>
        </div>
    );
};

interface TargetCreatorProps {
    selectedTargetType: TargetType;
    setSelectedTargetType: (type: TargetType) => void;
    isCreatingTarget: boolean;
    onToggleCreating?: (creating: boolean) => void;
    onCreateAtCamera: () => void;
}

const TargetCreator: React.FC<TargetCreatorProps> = ({
    selectedTargetType,
    setSelectedTargetType,
    isCreatingTarget,
    onToggleCreating,
    onCreateAtCamera
}) => {
    return (
        <div style={{
            marginTop: '8px',
            padding: '8px',
            border: '1px solid #ddd',
            borderRadius: '6px',
            background: '#ffffff',
            width: '100%',
            boxSizing: 'border-box'
        }}>
            <h3 style={{marginTop: 0, marginBottom: 8, fontSize: '14px', textAlign: 'center'}}>创建目标物</h3>
            <div style={{
                display: 'flex',
                flexDirection: 'column',
                gap: '8px',
                alignItems: 'center'
            }}>
                <div>
                    <label style={{marginRight: '8px', fontSize: 12}}>类型:</label>
                    <select
                        value={selectedTargetType}
                        onChange={(e) => setSelectedTargetType(e.target.value as TargetType)}
                        style={{padding: '4px 6px', marginRight: '6px', fontSize: 12}}
                    >
                        <option value="RECT">矩形</option>
                        <option value="CIRCLE">圆形</option>
                        <option value="CYLINDER">圆柱体</option>
                    </select>
                </div>
                <div style={{display: 'flex', gap: '8px', flexWrap: 'wrap', justifyContent: 'center'}}>
                    <button
                        onClick={() => onToggleCreating?.(!isCreatingTarget)}
                        style={{
                            padding: '4px 8px',
                            fontSize: '12px',
                            backgroundColor: isCreatingTarget ? '#ff6b6b' : '#4ecdc4',
                            color: 'white',
                            border: 'none',
                            borderRadius: '4px',
                            cursor: 'pointer'
                        }}
                    >
                        {isCreatingTarget ? '取消' : '开始创建'}
                    </button>
                    <button
                        onClick={onCreateAtCamera}
                        style={{
                            padding: '4px 8px',
                            fontSize: '12px',
                            backgroundColor: '#45b7d1',
                            color: 'white',
                            border: 'none',
                            borderRadius: '4px',
                            cursor: 'pointer'
                        }}
                    >
                        在车前创建
                    </button>
                </div>
                <div style={{fontSize: '11px', color: '#374151', textAlign: 'center'}}>
                    状态: {isCreatingTarget ? '就绪 - 点击画布创建' : '未激活'}
                </div>
            </div>
        </div>
    );
};

export default TargetManager;
