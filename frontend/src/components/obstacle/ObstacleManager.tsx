import {useState} from "react";
import type {Obstacle} from "../../model/obstacle";
import {useObstacleStore} from "../../store/obstacleStore";
import type {ObstacleType} from "../../model/obstacle";

interface ObstacleManagerProps {
    onCreateInFront?: (x: number, y: number) => void;
    isCreatingObstacle?: boolean;
    onToggleCreating?: (creating: boolean) => void;
    selectedObstacleType?: ObstacleType;
    onObstacleTypeChange?: (type: ObstacleType) => void;
}

export const ObstacleManager: React.FC<ObstacleManagerProps> = ({
    onCreateInFront,
    isCreatingObstacle: externalIsCreating,
    onToggleCreating,
    selectedObstacleType: externalSelectedType,
    onObstacleTypeChange
}) => {
    const {
        obstacles,
        updateObstacle,
        removeObstacle,
        selectObstacle,
        selectedObstacleId
    } = useObstacleStore();

    const [editingObstacle, setEditingObstacle] = useState<Obstacle | null>(null);
    const [editForm, setEditForm] = useState<Partial<Obstacle>>({});
    
    const isCreatingObstacle = externalIsCreating !== undefined ? externalIsCreating : false;
    const selectedObstacleType = externalSelectedType || 'RECT';
    const setSelectedObstacleType = (type: ObstacleType) => {
        onObstacleTypeChange?.(type);
    };

    const handleSaveEdit = () => {
        if (editingObstacle) {
            updateObstacle(editingObstacle.id, editForm);
            setEditingObstacle(null);
            setEditForm({});
        }
    };

    const handleCancelEdit = () => {
        setEditingObstacle(null);
        setEditForm({});
    };

    const handleDelete = (id: string) => {
        removeObstacle(id);
        if (selectedObstacleId === id) {
            selectObstacle(null);
        }
    };

    const handleStartEdit = (obs: Obstacle) => {
        setEditingObstacle(obs);
        setEditForm({...obs});
    };

    const handleCreateObstacleAtCamera = () => {
        if (onCreateInFront) {
            onCreateInFront(0, 0);
        }
    };

    return (
        <div style={{
            flex: '0 0 20%',
            minWidth: '250px',
            border: '2px solid #333',
            borderRadius: '8px',
            padding: '15px',
            background: '#f9f9f9',
            overflowY: 'auto',
            maxHeight: '570px'
        }}>
            <h3 style={{marginTop: 0, marginBottom: '15px'}}>障碍物管理</h3>
            
            <div style={{marginTop: '15px'}}>
                <h4 style={{marginTop: 0, marginBottom: '10px', fontSize: '14px'}}>已有障碍物列表</h4>
                <div style={{
                    border: '1px solid rgb(221, 221, 221)',
                    borderRadius: '4px',
                    padding: '8px',
                    maxHeight: '300px',
                    overflowY: 'auto',
                    background: 'rgb(255, 255, 255)'
                }}>
                    {editingObstacle ? (
                        <ObstacleEditForm
                            editingObstacle={editingObstacle}
                            editForm={editForm}
                            setEditForm={setEditForm}
                            onSave={handleSaveEdit}
                            onCancel={handleCancelEdit}
                        />
                    ) : (
                        obstacles.length > 0 ? (
                            <div style={{
                                display: 'grid',
                                gridTemplateColumns: '1fr 1fr',
                                gap: '10px'
                            }}>
                                {obstacles.map(obs => (
                                    <ObstacleItem
                                        key={obs.id}
                                        obstacle={obs}
                                        isSelected={obs.id === selectedObstacleId}
                                        onSelect={() => selectObstacle(obs.id)}
                                        onEdit={() => handleStartEdit(obs)}
                                        onDelete={() => handleDelete(obs.id)}
                                    />
                                ))}
                            </div>
                        ) : (
                            <div style={{textAlign: 'center', color: '#999', padding: '15px', fontSize: '13px'}}>
                                暂无障碍物数据
                            </div>
                        )
                    )}
                </div>
            </div>

            <ObstacleCreator
                selectedObstacleType={selectedObstacleType}
                setSelectedObstacleType={setSelectedObstacleType}
                isCreatingObstacle={isCreatingObstacle}
                onToggleCreating={onToggleCreating}
                onCreateAtCamera={handleCreateObstacleAtCamera}
            />
        </div>
    );
};

interface ObstacleEditFormProps {
    editingObstacle: Obstacle;
    editForm: Partial<Obstacle>;
    setEditForm: (form: Partial<Obstacle>) => void;
    onSave: () => void;
    onCancel: () => void;
}

const ObstacleEditForm: React.FC<ObstacleEditFormProps> = ({
    editingObstacle,
    editForm,
    setEditForm,
    onSave,
    onCancel
}) => {
    return (
        <div style={{
            padding: '12px',
            border: '1px solid #4ecdc4',
            borderRadius: '4px',
            background: 'rgb(255, 255, 255)',
            boxShadow: '0 2px 4px rgba(0,0,0,0.1)',
            marginBottom: '10px'
        }}>
            <h5 style={{marginTop: 0, marginBottom: '10px', color: '#4ecdc4'}}>编辑障碍物</h5>
            <div style={{display: 'flex', flexDirection: 'column', gap: '8px', marginTop: '10px'}}>
                <div>
                    <label style={{fontSize: '12px', marginRight: '5px'}}>X坐标: </label>
                    <input 
                        type="number" 
                        value={editForm.x} 
                        onChange={(e) => setEditForm({...editForm, x: parseInt(e.target.value)})} 
                        style={{fontSize: '12px', padding: '3px 5px', width: '80px'}}
                    />
                </div>
                <div>
                    <label style={{fontSize: '12px', marginRight: '5px'}}>Y坐标: </label>
                    <input 
                        type="number" 
                        value={editForm.y} 
                        onChange={(e) => setEditForm({...editForm, y: parseInt(e.target.value)})} 
                        style={{fontSize: '12px', padding: '3px 5px', width: '80px'}}
                    />
                </div>
                {editingObstacle.type === 'RECT' && (
                    <>
                        <div>
                            <label style={{fontSize: '12px', marginRight: '5px'}}>宽度: </label>
                            <input 
                                type="number" 
                                value={editForm.w} 
                                onChange={(e) => setEditForm({...editForm, w: parseInt(e.target.value)})} 
                                style={{fontSize: '12px', padding: '3px 5px', width: '60px'}}
                            />
                        </div>
                        <div>
                            <label style={{fontSize: '12px', marginRight: '5px'}}>高度: </label>
                            <input 
                                type="number" 
                                value={editForm.h} 
                                onChange={(e) => setEditForm({...editForm, h: parseInt(e.target.value)})} 
                                style={{fontSize: '12px', padding: '3px 5px', width: '60px'}}
                            />
                        </div>
                    </>
                )}
                {editingObstacle.type === 'CIRCLE' && (
                    <div>
                        <label style={{fontSize: '12px', marginRight: '5px'}}>半径: </label>
                        <input 
                            type="number" 
                            value={editForm.r} 
                            onChange={(e) => setEditForm({...editForm, r: parseInt(e.target.value)})} 
                            style={{fontSize: '12px', padding: '3px 5px', width: '60px'}}
                        />
                    </div>
                )}
                {editingObstacle.type === 'RECT' && (
                    <div>
                        <label style={{fontSize: '12px', marginRight: '5px'}}>旋转角度: </label>
                        <input 
                            type="number" 
                            step="0.1" 
                            value={editForm.angle || 0} 
                            onChange={(e) => setEditForm({...editForm, angle: parseFloat(e.target.value)})} 
                            style={{fontSize: '12px', padding: '3px 5px', width: '80px'}}
                        />
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

interface ObstacleItemProps {
    obstacle: Obstacle;
    isSelected: boolean;
    onSelect: () => void;
    onEdit: () => void;
    onDelete: () => void;
}

const ObstacleItem: React.FC<ObstacleItemProps> = ({
    obstacle,
    isSelected,
    onSelect,
    onEdit,
    onDelete
}) => {
    return (
        <div 
            style={{
                border: `1px solid ${isSelected ? '#4ecdc4' : 'rgb(221, 221, 221)'}`,
                borderRadius: '4px',
                padding: '5px',
                background: isSelected ? 'rgba(78, 205, 196, 0.1)' : 'rgb(255, 255, 255)',
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
                <div style={{width: '12px', height: '12px', backgroundColor: obstacle.color, borderRadius: '2px', border: '1px solid #ddd'}} />
                <span style={{fontSize: '10px', color: '#666', fontWeight: '500'}}>
                    {obstacle.type === 'RECT' ? '矩形' : '圆形'}
                </span>
            </div>
            
            <div style={{fontSize: '9px', color: '#999', marginBottom: '1px', whiteSpace: 'normal', wordBreak: 'break-all', lineHeight: '1.1'}}>
                ID: {obstacle.id}
            </div>
            
            <div style={{fontSize: '9px', color: '#666', marginBottom: '1px', lineHeight: '1.1'}}>
                位置: ({Math.floor(obstacle.x)}, {Math.floor(obstacle.y)})
            </div>
            
            <div style={{fontSize: '9px', color: '#666', marginBottom: '1px', lineHeight: '1.1'}}>
                {obstacle.type === 'RECT' ? `尺寸: ${obstacle.w}x${obstacle.h}` : `半径: ${obstacle.r}`}
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

interface ObstacleCreatorProps {
    selectedObstacleType: ObstacleType;
    setSelectedObstacleType: (type: ObstacleType) => void;
    isCreatingObstacle: boolean;
    onToggleCreating?: (creating: boolean) => void;
    onCreateAtCamera: () => void;
}

const ObstacleCreator: React.FC<ObstacleCreatorProps> = ({
    selectedObstacleType,
    setSelectedObstacleType,
    isCreatingObstacle,
    onToggleCreating,
    onCreateAtCamera
}) => {
    return (
        <div style={{
            marginTop: '20px',
            padding: '15px',
            border: '2px solid #333',
            borderRadius: '8px',
            background: '#f9f9f9',
            width: '100%',
            maxWidth: '280px'
        }}>
            <h3 style={{marginTop: 0, fontSize: '16px', textAlign: 'center'}}>创建障碍物</h3>
            <div style={{
                display: 'flex',
                flexDirection: 'column',
                gap: '10px',
                alignItems: 'center'
            }}>
                <div>
                    <label style={{marginRight: '10px'}}>选择类型:</label>
                    <select 
                        value={selectedObstacleType} 
                        onChange={(e) => setSelectedObstacleType(e.target.value as ObstacleType)}
                        style={{padding: '5px', marginRight: '10px'}}
                    >
                        <option value="RECT">矩形</option>
                        <option value="CIRCLE">圆形</option>
                    </select>
                </div>
                <div style={{display: 'flex', gap: '10px'}}>
                    <button 
                        onClick={() => onToggleCreating?.(!isCreatingObstacle)}
                        style={{
                            padding: '6px 12px',
                            fontSize: '14px',
                            backgroundColor: isCreatingObstacle ? '#ff6b6b' : '#4ecdc4',
                            color: 'white',
                            border: 'none',
                            borderRadius: '4px',
                            cursor: 'pointer'
                        }}
                    >
                        {isCreatingObstacle ? '取消' : '开始创建'}
                    </button>
                    <button 
                        onClick={onCreateAtCamera}
                        style={{
                            padding: '6px 12px',
                            fontSize: '14px',
                            backgroundColor: '#45b7d1',
                            color: 'white',
                            border: 'none',
                            borderRadius: '4px',
                            cursor: 'pointer'
                        }}
                    >
                        在摄像头下创建
                    </button>
                </div>
                <div style={{fontSize: '12px', color: '#555', textAlign: 'center'}}>
                    状态: {isCreatingObstacle ? '就绪 - 点击画布创建' : '未激活'}
                </div>
            </div>
        </div>
    );
};

export default ObstacleManager;
